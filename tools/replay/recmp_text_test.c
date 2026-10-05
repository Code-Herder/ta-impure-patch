#include "tagpu_recmp_text.h"
#include "tagpu_recmp_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static RecMpRoster roster(unsigned source)
{
    RecMpRoster r={ .version=1, .count=10, .each=2, .slots=21 };
    for (unsigned i=0;i<10;i++) r.actor[i]=(RecMpSeat){11+11*i,source ? (i*7+3)%10 : i,1+2*i,2+2*i};
    return r;
}
static RecMpHeader header(unsigned origin, unsigned sequence, unsigned tick, unsigned kind, unsigned bytes)
{
    RecMpHeader h={ .version=RECMP_VERSION, .header=sizeof h, .bytes=sizeof h+bytes,
        .origin=origin, .sequence=sequence, .tick=tick, .kind=kind,
        .match={1}, .stream={ .Data1=origin } };
    memcpy(h.magic,"IMPUREMP",8); return h;
}
static RecMpSeatMaps* maps(const RecMpRoster* destination)
{
    RecMpRoster r=roster(1);
    RecMpSeatMaps* m=tagpu_recmp_seats_begin(destination); CHECK(m);
    for (unsigned i=0;i<10;i++) {
        RecMpHeader h=header(r.actor[i].origin,1,0,RECMP_SEATS,sizeof r);
        CHECK(tagpu_recmp_seats_put(m,&h,&r,sizeof r));
    }
    return m;
}
static RecMpTextEntry entry(unsigned phase, unsigned origin, unsigned recipient, uint64_t event,
                            unsigned tick, unsigned mode)
{
    RecMpTextEntry e={ .phase=phase, .mode=mode, .origin=origin, .recipient=recipient,
                      .source_tick=tick, .display=phase==RECMP_TEXT_RECEIVED ? 8 : 4, .event=event };
    e.stock[0]=5; memset(e.stock+1,'T',64);
    if (phase==RECMP_TEXT_SOURCE && mode) { e.target[0]=22; e.target[1]=55; }
    return e;
}
static unsigned batch(unsigned char* b, const RecMpTextEntry* e, unsigned count)
{
    memcpy(b,&count,4); memcpy(b+4,e,count*sizeof *e); return 4+count*sizeof *e;
}
static void codec(void)
{
    RecMpRoster r=roster(0);
    RecMpSeatMaps* m=maps(&r); RecMpText* c=tagpu_recmp_text_begin(&r);
    RecMpTextEntry e=entry(RECMP_TEXT_RECEIVED,11,22,1,0,RECMP_TEXT_MODE_UNKNOWN);
    unsigned char b[RECMP_BODY_MAX+1], original[RECMP_BODY_MAX+1];
    unsigned bytes=batch(b,&e,1);
    RecMpHeader h=header(22,2,100,RECMP_TEXT,bytes);
    RecMpTextState before, after;
    CHECK(c && tagpu_recmp_text_complete(c));
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,200) && !tagpu_recmp_text_complete(c));
    CHECK(tagpu_recmp_text_get(c,22,&before) && before.source_tick==100 && before.applied_tick==200);
    memset(b,0xCC,bytes);
    CHECK(tagpu_recmp_text_get(c,22,&after) && !memcmp(&before,&after,sizeof before));
    /* A receipt before its original must close against byte-exact original data. */
    RecMpTextEntry source=entry(RECMP_TEXT_SOURCE,11,11,1,0,3);
    bytes=batch(b,&source,1); h=header(11,2,0,RECMP_TEXT,bytes);
    b[4+offsetof(RecMpTextEntry,stock)+64]^=1;
    CHECK(!tagpu_recmp_text_put(c,m,&h,b,bytes,201) && !tagpu_recmp_text_complete(c));
    bytes=batch(b,&source,1);
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,201) && tagpu_recmp_text_complete(c));
    /* Identical text is a second event, even when the source clock stays zero. */
    source.event=2; bytes=batch(b,&source,1); h.sequence=3;
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,202));
    e.event=2; bytes=batch(b,&e,1); h=header(22,3,101,RECMP_TEXT,bytes);
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,203));
    /* Native delivery may repeat or reorder identities. Keep each observation. */
    RecMpTextEntry observed[3]={e,e,e}; observed[1].event=1;
    bytes=batch(b,observed,3); h=header(22,4,102,RECMP_TEXT,bytes);
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,204) && tagpu_recmp_text_complete(c));
    CHECK(tagpu_recmp_text_get(c,22,&before) && before.count==3);
    memcpy(original,b,bytes);
    for (unsigned control=0;control<28;control++) {
        unsigned n=bytes, applied=205; RecMpHeader bad=header(22,5,103,RECMP_TEXT,bytes);
        memcpy(b,original,bytes); RecMpTextEntry changed=observed[2];
        switch (control) {
        case 0: b[0]=0; break; case 1: b[0]=RECMP_TEXT_COUNT+1; break;
        case 2: changed.phase=0; break; case 3: changed.mode=0; break;
        case 4: changed.reserved=1; break; case 5: changed.origin=999; break;
        case 6: changed.recipient=55; break; case 7: changed.source_tick++; break;
        case 8: changed.display=4; break; case 9: changed.event=0; break;
        case 10: changed.target[0]=22; break; case 11: changed.stock[0]=6; break;
        case 12: changed.stock[1]=0; break; case 13: changed.stock[64]^=1; break;
        case 14: changed.padding[2]=1; break; case 15: n--; break;
        case 16: n++; b[bytes]=0; break; case 17: bad.origin=55; break;
        case 18: bad.sequence=4; break; case 19: bad.tick=101; break;
        case 20: bad.kind=RECMP_ROOTS; break; case 21: bad.version--; break;
        case 22: bad.header--; break; case 23: bad.stream.Data1=55; break;
        case 24: bad.match.Data1=2; break; case 25: bad.magic[0]=0; break;
        case 26: applied=203; break; case 27: bad.match.Data1=0; break;
        }
        if (control>=2 && control<=14) memcpy(b+4+2*sizeof changed,&changed,sizeof changed);
        bad.bytes=sizeof bad+n;
        if (tagpu_recmp_text_put(c,m,&bad,b,n,applied)) {
            fprintf(stderr,"accepted malformed control %u\n",control); exit(1);
        }
        CHECK(tagpu_recmp_text_get(c,22,&after) && !memcmp(&before,&after,sizeof before));
        CHECK(tagpu_recmp_text_complete(c));
    }
    unsigned char* page=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); DWORD old;
    CHECK(page && VirtualProtect(page+4096,4096,PAGE_NOACCESS,&old));
    for (unsigned n=0;n<bytes;n++) {
        unsigned char* p=page+4096-n; memcpy(p,original,n);
        h=header(22,5,103,RECMP_TEXT,n);
        CHECK(!tagpu_recmp_text_put(c,m,&h,p,n,205));
        CHECK(tagpu_recmp_text_get(c,22,&after) && !memcmp(&before,&after,sizeof before));
    }
    VirtualFree(page,0,MEM_RELEASE);
    /* Refusal in a later entry cannot reserve the earlier entry's new identity. */
    observed[0].event=3; observed[1].event=4; observed[2].stock[64]^=1;
    bytes=batch(b,observed,3); h=header(22,5,103,RECMP_TEXT,bytes);
    CHECK(!tagpu_recmp_text_put(c,m,&h,b,bytes,205) && tagpu_recmp_text_complete(c));
    source.event=3; source.stock[64]='Z'; bytes=batch(b,&source,1);
    h=header(11,4,0,RECMP_TEXT,bytes);
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,206) && tagpu_recmp_text_complete(c));
    e=entry(RECMP_TEXT_RECEIVED,11,33,1,0,RECMP_TEXT_MODE_UNKNOWN);
    bytes=batch(b,&e,1); h=header(33,2,0,RECMP_TEXT,bytes);
    CHECK(!tagpu_recmp_text_put(c,m,&h,b,bytes,207) && tagpu_recmp_text_complete(c));
    e=entry(RECMP_TEXT_RECEIVED,44,22,9,0,RECMP_TEXT_MODE_UNKNOWN);
    bytes=batch(b,&e,1); h=header(22,5,103,RECMP_TEXT,bytes);
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,207) && !tagpu_recmp_text_complete(c));
    source=entry(RECMP_TEXT_SOURCE,44,44,9,0,3); source.target[0]=55; source.target[1]=0;
    bytes=batch(b,&source,1); h=header(44,2,0,RECMP_TEXT,bytes);
    CHECK(!tagpu_recmp_text_put(c,m,&h,b,bytes,208) && !tagpu_recmp_text_complete(c));
    source.target[0]=22; source.target[1]=55; bytes=batch(b,&source,1);
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,208) && tagpu_recmp_text_complete(c));
    /* Every actor can contribute under a shuffled seat mapping. */
    for (unsigned i=1;i<10;i++) {
        source=entry(RECMP_TEXT_SOURCE,11+11*i,11+11*i,UINT64_MAX,UINT32_MAX,i%4);
        bytes=batch(b,&source,1); h=header(source.origin,20,UINT32_MAX,RECMP_TEXT,bytes);
        CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,UINT32_MAX));
    }
    CHECK(tagpu_recmp_text_complete(c));
    tagpu_recmp_text_end(c); tagpu_recmp_seats_end(m);
    printf("PASS: ten origins, exact full-length/repeated text, receipt-before-source, reordered/repeated receipts, 28 malformed and %u guarded refusals\n",4u+3u*(unsigned)sizeof e);
}
static void recorded(RecMpHistory* history, FILE* file, const RecMpHeader* h,
                      const void* body, unsigned bytes, unsigned tick, unsigned phase)
{
    RecMpRecord where={ .tick=tick, .phase=phase };
    if (phase!=RECMP_RECORD_PROOF) where.ordinal=tagpu_recmp_history_ledger(history)->records+1;
    CHECK(tagpu_recmp_history_record(history,&where,h,body,bytes));
    if (file) {
        CHECK(fwrite(&where,sizeof where,1,file)==1 && fwrite(h,sizeof *h,1,file)==1);
        CHECK(!bytes || fwrite(body,bytes,1,file)==1);
    }
}
static void history(unsigned original)
{
    RecMpRoster r=roster(0), source=roster(1);
    RecMpRecordHeader file={ .version=1, .count=10, .recorder=11, .match={1} };
    for (unsigned i=0;i<10;i++) file.actor[i]=(RecMpRecordActor){r.actor[i].origin,i==0};
    RecMpHistory* c=tagpu_recmp_history_begin(&file,&r,2,2); CHECK(c);
    FILE* dump=fopen(original ? "text-history.bin" : "text-history-missing.bin","wb"); CHECK(dump);
    CHECK(fwrite(&file,sizeof file,1,dump)==1 && fwrite(&r,sizeof r,1,dump)==1);
    for (unsigned i=0;i<10;i++) {
        RecMpHeader h=header(r.actor[i].origin,1,0,RECMP_SEATS,sizeof source);
        recorded(c,dump,&h,&source,sizeof source,200,i ? RECMP_RECORD_CONSUMED : RECMP_RECORD_PRODUCED);
    }
    RecMpTextEntry e=entry(RECMP_TEXT_RECEIVED,55,11,123,0,RECMP_TEXT_MODE_UNKNOWN);
    unsigned char body[4+sizeof e]; unsigned bytes=batch(body,&e,1);
    RecMpHeader h=header(11,2,1,RECMP_TEXT,bytes);
    recorded(c,dump,&h,body,bytes,201,RECMP_RECORD_PRODUCED);
    RecMpTextState before, after;
    CHECK(tagpu_recmp_history_text(c,11,&before) && before.source_tick==1 && before.applied_tick==201);
    CHECK(!tagpu_recmp_history_text(c,55,&after));
    RecMpSeal seal;
    CHECK(tagpu_recmp_stream_seal(&tagpu_recmp_history_ledger(c)->source[0],2,1,&seal));
    h=header(11,3,2,RECMP_SEAL,sizeof seal);
    recorded(c,dump,&h,&seal,sizeof seal,202,RECMP_RECORD_PRODUCED);
    if (original) {
        e=entry(RECMP_TEXT_SOURCE,55,55,123,0,3); e.target[0]=11; e.target[1]=0;
        bytes=batch(body,&e,1); h=header(55,2,0,RECMP_TEXT,bytes);
        recorded(c,dump,&h,body,bytes,0,RECMP_RECORD_PROOF);
    }
    for (unsigned i=1;i<10;i++) {
        const RecMpStream* stream=&tagpu_recmp_history_ledger(c)->source[i];
        CHECK(tagpu_recmp_stream_seal(stream,1,0,&seal));
        h=header(r.actor[i].origin,stream->sequence+1,1,RECMP_SEAL,sizeof seal);
        recorded(c,dump,&h,&seal,sizeof seal,0,RECMP_RECORD_PROOF);
    }
    CHECK(tagpu_recmp_history_text(c,11,&after) && !memcmp(&before,&after,sizeof before));
    CHECK(!tagpu_recmp_history_text(c,55,&after));
    const RecMpFileState* ledger=tagpu_recmp_history_ledger(c);
    RecMpRecordEnd end={ .version=1, .count=10, .records=ledger->records };
    memcpy(end.cut,ledger->cut,sizeof end.cut);
    CHECK(tagpu_recmp_file_footer(ledger,&end));
    CHECK(tagpu_recmp_history_footer(c,&end)==(int)original);
    CHECK(fclose(dump)==0);
    dump=fopen(original ? "text-history-end.bin" : "text-history-missing-end.bin","wb"); CHECK(dump);
    CHECK(fwrite(&end,sizeof end,1,dump)==1 && fclose(dump)==0);
    tagpu_recmp_history_end(c);
    printf("PASS: persisted source proof %s; proof never publishes original text or changes recipient observation\n",original ? "joins original" : "refuses missing original");
}
static void capacity(void)
{
    RecMpRoster r=roster(0); RecMpSeatMaps* m=maps(&r); RecMpText* c=tagpu_recmp_text_begin(&r);
    RecMpTextEntry e[RECMP_TEXT_COUNT]; unsigned char b[RECMP_BODY_MAX];
    unsigned total=0, sequence=2; CHECK(c);
    while (total<RECMP_TEXT_EVENTS) {
        unsigned n=RECMP_TEXT_EVENTS-total; if (n>RECMP_TEXT_COUNT) n=RECMP_TEXT_COUNT;
        for (unsigned i=0;i<n;i++) e[i]=entry(RECMP_TEXT_SOURCE,11,11,total+i+1,0,0);
        unsigned bytes=batch(b,e,n); RecMpHeader h=header(11,sequence++,0,RECMP_TEXT,bytes);
        CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,0)); total+=n;
    }
    CHECK(tagpu_recmp_text_complete(c));
    e[0]=entry(RECMP_TEXT_RECEIVED,11,22,RECMP_TEXT_EVENTS+1,0,RECMP_TEXT_MODE_UNKNOWN);
    unsigned bytes=batch(b,e,1); RecMpHeader h=header(22,2,1,RECMP_TEXT,bytes);
    CHECK(!tagpu_recmp_text_put(c,m,&h,b,bytes,1) && tagpu_recmp_text_complete(c));
    e[0].event=RECMP_TEXT_EVENTS; bytes=batch(b,e,1);
    CHECK(tagpu_recmp_text_put(c,m,&h,b,bytes,1) && tagpu_recmp_text_complete(c));
    tagpu_recmp_text_end(c); tagpu_recmp_seats_end(m);
    puts("PASS: 65536 identity bound, collisions and existing-key receipt at capacity");
}
typedef struct { unsigned entries, calls, fail; } Sink;
static int emit(void* ctx, unsigned origin, unsigned tick, const void* body, unsigned bytes)
{
    Sink* s=ctx; unsigned count; const unsigned char* b=body;
    memcpy(&count,b,4); CHECK(count && count<=RECMP_TEXT_COUNT && bytes==4+count*sizeof(RecMpTextEntry));
    for (unsigned i=0;i<count;i++) {
        RecMpTextEntry e; memcpy(&e,b+4+i*sizeof e,sizeof e);
        CHECK(origin==11 && tick==7 && e.event==s->entries+i+1 && e.stock[64]=='T');
    }
    s->calls++; if (s->fail) return 0;
    s->entries+=count; return 1;
}
static void queue(void)
{
    RecMpTextQueue* q=tagpu_recmp_text_queue_begin(); Sink s={0}; unsigned tick=999; CHECK(q);
    CHECK(!tagpu_recmp_text_queue_tick(q,&tick) && tick==999);
    for (unsigned i=0;i<RECMP_TEXT_PENDING;i++) {
        RecMpTextEntry e=entry(RECMP_TEXT_SOURCE,11,11,i+1,7,0);
        CHECK(tagpu_recmp_text_queue_push(q,7,&e)); memset(&e,0xCC,sizeof e);
    }
    CHECK(tagpu_recmp_text_queue_flush(q,6,emit,&s) && !s.calls);
    CHECK(tagpu_recmp_text_queue_tick(q,&tick)==1 && tick==7);
    CHECK(tagpu_recmp_text_queue_flush(q,7,emit,&s) && s.entries==RECMP_TEXT_PENDING);
    tick=999; CHECK(!tagpu_recmp_text_queue_tick(q,&tick) && tick==999);
    for (unsigned i=0;i<17;i++) {
        RecMpTextEntry e=entry(RECMP_TEXT_SOURCE,11,11,RECMP_TEXT_PENDING+i+1,7,0);
        CHECK(tagpu_recmp_text_queue_push(q,7,&e));
    }
    CHECK(tagpu_recmp_text_queue_flush(q,7,emit,&s) && s.entries==RECMP_TEXT_PENDING+17);
    RecMpTextEntry e=entry(RECMP_TEXT_SOURCE,11,11,s.entries+1,7,0);
    CHECK(tagpu_recmp_text_queue_push(q,7,&e)); s.fail=1;
    CHECK(!tagpu_recmp_text_queue_flush(q,7,emit,&s)); s.fail=0;
    CHECK(!tagpu_recmp_text_queue_flush(q,7,emit,&s) && !tagpu_recmp_text_queue_push(q,7,&e));
    CHECK(tagpu_recmp_text_queue_tick(q,&tick)==-1 && tick==999);
    tagpu_recmp_text_queue_end(q);
    q=tagpu_recmp_text_queue_begin(); CHECK(q);
    for (unsigned i=0;i<RECMP_TEXT_PENDING;i++) CHECK(tagpu_recmp_text_queue_push(q,7,&e));
    CHECK(!tagpu_recmp_text_queue_push(q,7,&e) && !tagpu_recmp_text_queue_flush(q,7,emit,&s));
    tagpu_recmp_text_queue_end(q);
    puts("PASS: copied 4096-entry queue, tick cut, wrap, overflow and failed-publication refusal");
}
int main(void) { codec(); capacity(); queue(); history(0); history(1); return 0; }
