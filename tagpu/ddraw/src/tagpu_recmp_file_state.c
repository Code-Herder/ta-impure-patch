#include "tagpu_recmp_file.h"
#include <string.h>

int tagpu_recmp_file_roster_valid(const RecMpRecordHeader* h, const RecMpRoster* r)
{
    RecMpFileState state;
    unsigned i;
    if (!tagpu_recmp_roster_valid(r) || !tagpu_recmp_file_state(&state,h) || h->count!=r->count)
        return 0;
    for (i=0;i<r->count;i++) if (h->actor[i].origin!=r->actor[i].origin) return 0;
    return 1;
}

int tagpu_recmp_file_state(RecMpFileState* s, const RecMpRecordHeader* h)
{
    unsigned i, k, own=0;
    GUID zero={0};
    if (!s || !h || h->version!=1 || h->reserved || !h->count || h->count>RECMP_ACTORS ||
        !memcmp(&h->match,&zero,sizeof zero)) return 0;
    for (i=0;i<RECMP_ACTORS;i++) {
        if (i>=h->count) {
            if (h->actor[i].origin || h->actor[i].local) return 0;
            continue;
        }
        if (!h->actor[i].origin || h->actor[i].local>1) return 0;
        for (k=0;k<i;k++) if (h->actor[k].origin==h->actor[i].origin) return 0;
        if (h->actor[i].origin==h->recorder && h->actor[i].local) own=1;
    }
    if (!own) return 0;
    memset(s,0,sizeof *s); s->header=*h;
    for (i=0;i<h->count;i++) if (!h->actor[i].local) s->cut[i].origin=h->actor[i].origin;
    return 1;
}

int tagpu_recmp_file_record(RecMpFileState* s, const RecMpRecord* where,
                            const RecMpHeader* h, const void* body, unsigned bytes)
{
    RecMpStream next;
    GUID zero={0};
    unsigned i, local, proof;
    if (!s || !where || !h || bytes>RECMP_BODY_MAX || (bytes && !body) ||
        memcmp(h->magic,"IMPUREMP",8) || h->version!=RECMP_VERSION ||
        h->header!=sizeof *h || h->bytes!=sizeof *h+bytes ||
        memcmp(&h->match,&s->header.match,sizeof h->match) ||
        !memcmp(&h->stream,&zero,sizeof zero)) return 0;
    for (i=0;i<s->header.count;i++) if (s->header.actor[i].origin==h->origin) break;
    if (i==s->header.count) return 0;
    local=s->header.actor[i].local; proof=where->phase==RECMP_RECORD_PROOF;
    if (proof) {
        if (local || where->ordinal || where->tick) return 0;
    } else if (s->proof || s->records==UINT64_MAX || where->ordinal!=s->records+1 ||
        where->tick<s->tick || where->phase!=(local ? RECMP_RECORD_PRODUCED : RECMP_RECORD_CONSUMED) ||
        (!local && s->consumed==UINT64_MAX)) return 0;
    if (s->source[i].sequence && memcmp(&s->stream[i],&h->stream,sizeof h->stream)) return 0;
    next=s->source[i];
    if (!tagpu_recmp_stream_put(&next,h,body,bytes)) return 0;
    s->source[i]=next; s->stream[i]=h->stream;
    if (proof) s->proof=1;
    else {
        s->records=where->ordinal; s->tick=where->tick;
        if (!local) {
            RecMpCutPoint* cut=&s->cut[i];
            cut->sequence=h->sequence; cut->source_tick=h->tick;
            cut->application_tick=where->tick; cut->application_order=++s->consumed;
            cut->stream=h->stream;
        }
    }
    return 1;
}

int tagpu_recmp_file_block(RecMpFileState* s, const void* body, unsigned bytes)
{
    const unsigned char* b=body;
    RecMpFileState next;
    unsigned at=0;
    if (!s || !b || !bytes) return 0;
    next=*s;
    while (at<bytes) {
        RecMpRecord where;
        RecMpHeader h;
        if (bytes-at<sizeof where+sizeof h) return 0;
        memcpy(&where,b+at,sizeof where); at+=sizeof where;
        memcpy(&h,b+at,sizeof h);
        if (h.bytes<sizeof h || h.bytes>RECMP_PACKET_MAX || h.bytes>bytes-at ||
            !tagpu_recmp_file_record(&next,&where,&h,b+at+sizeof h,h.bytes-sizeof h)) return 0;
        at+=h.bytes;
    }
    *s=next;
    return 1;
}

int tagpu_recmp_file_footer(const RecMpFileState* s, const RecMpRecordEnd* end)
{
    unsigned i;
    if (!s || !end || end->version!=1 || end->count!=s->header.count || end->records!=s->records ||
        memcmp(end->cut,s->cut,sizeof end->cut)) return 0;
    for (i=0;i<s->header.count;i++) {
        const RecMpStream* source=&s->source[i];
        if (s->header.actor[i].local) {
            if (!source->ended) return 0;
        } else if (!source->has_seal || source->sealed_sequence<s->cut[i].sequence) return 0;
    }
    return 1;
}
