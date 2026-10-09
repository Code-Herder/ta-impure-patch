#include "tagpu_recmp_explored.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("FAIL %u: %s\n",__LINE__,#x); exit(1); } } while (0)

static RecMpHeader header(unsigned origin,unsigned seq,unsigned kind,unsigned tick,unsigned bytes)
{
    RecMpHeader h={0}; memcpy(h.magic,"IMPUREMP",8); h.version=RECMP_VERSION;
    h.header=sizeof h; h.bytes=sizeof h+bytes; h.origin=origin; h.sequence=seq;
    h.tick=tick; h.kind=kind; h.stream.Data1=origin; h.match.Data1=7; return h;
}
static RecMpRoster roster(unsigned count,int reverse)
{
    RecMpRoster r={1,count,101,10,{{0}}};
    for (unsigned i=0;i<count;i++) r.actor[i]=(RecMpSeat){100+i,reverse ? count-1-i : i,1+10*i,10+10*i};
    return r;
}
static RecMpSeatMaps* maps(RecMpRoster* destination,RecMpRoster* source)
{
    RecMpSeatMaps* m=tagpu_recmp_seats_begin(destination); CHECK(m);
    for (unsigned i=0;i<source->count;i++) {
        RecMpHeader h=header(source->actor[i].origin,1,RECMP_SEATS,1,sizeof *source);
        CHECK(tagpu_recmp_seats_put(m,&h,source,sizeof *source));
    }
    return m;
}
typedef struct {
    RecMpExplored* cache; RecMpSeatMaps* seats;
    unsigned sequence,tick,applied,calls,fail_call,body_bytes;
    unsigned char* erase;
    unsigned char previous[5120]; int previous_valid;
} Sink;
static int emit(void* ctx,unsigned origin,const void* raw,unsigned bytes)
{
    Sink* s=ctx; RecMpExploreChunk chunk; RecMpExploreState before;
    unsigned char image[5120];
    memcpy(&chunk,raw,sizeof chunk); s->calls++;
    if (s->calls==s->fail_call) return 0;
    RecMpHeader h=header(origin,++s->sequence,RECMP_EXPLORED,s->tick,bytes);
    CHECK(tagpu_recmp_explored_put(s->cache,s->seats,&h,raw,bytes,s->applied++));
    CHECK(tagpu_recmp_explored_idle(s->cache,origin)==!!(chunk.flags&RECMP_EXPLORE_END));
    if (!(chunk.flags&RECMP_EXPLORE_END)) {
        CHECK(tagpu_recmp_explored_get(s->cache,origin,&before,image,sizeof image)==s->previous_valid);
        if (s->previous_valid) CHECK(!memcmp(s->previous,image,s->body_bytes));
    }
    if (s->erase) { memset(s->erase,0xEE,s->body_bytes); s->erase=NULL; }
    return 1;
}
static void atomic_refusal(RecMpExplored* c,RecMpSeatMaps* m,RecMpHeader h,void* body,unsigned bytes)
{
    RecMpExploreState a,b; unsigned char x[5120],y[5120];
    int valid=tagpu_recmp_explored_get(c,100,&a,x,sizeof x);
    CHECK(!tagpu_recmp_explored_put(c,m,&h,body,bytes,900));
    CHECK(tagpu_recmp_explored_get(c,100,&b,y,sizeof y)==valid);
    if (valid) CHECK(!memcmp(&a,&b,sizeof a) && !memcmp(x,y,a.bytes));
}
int main(void)
{
    RecMpRoster destination=roster(10,0),source=roster(10,1);
    RecMpSeatMaps* m=maps(&destination,&source);
    RecMpExplored* c=tagpu_recmp_explored_begin(&destination,512,320); CHECK(c);
    unsigned char input[5120],wanted[5120],out[5120]; RecMpExploreState state;
    Sink s={0}; s.cache=c; s.seats=m; s.sequence=1; s.tick=2; s.applied=10; s.body_bytes=sizeof input;
    for (unsigned i=0;i<sizeof input;i++) input[i]=(unsigned char)(i*17);
    memcpy(wanted,input,sizeof wanted); s.erase=input;
    CHECK(tagpu_recmp_explored_encode(c,100,input,sizeof input,4,emit,&s));
    CHECK(s.calls==2 && tagpu_recmp_explored_get(c,100,&state,out,sizeof out));
    CHECK(state.cells==40960 && state.bytes==5120 && state.sequence==3 && state.source_tick==2 && state.applied_tick==11);
    CHECK(!memcmp(wanted,out,sizeof out));
    unsigned calls=s.calls;
    CHECK(tagpu_recmp_explored_encode(c,100,wanted,sizeof wanted,4,emit,&s) && s.calls==calls);
    s.tick++; s.previous_valid=1; memcpy(s.previous,wanted,sizeof wanted);
    for (unsigned i=0;i<sizeof wanted;i++) wanted[i]^=0x80;
    CHECK(tagpu_recmp_explored_encode(c,100,wanted,sizeof wanted,4,emit,&s));
    CHECK(s.calls==calls+2 && tagpu_recmp_explored_get(c,100,&state,out,sizeof out) && !memcmp(wanted,out,sizeof out));
    calls=s.calls; s.tick++;
    CHECK(tagpu_recmp_explored_encode(c,100,wanted,sizeof wanted,0xFFFF,emit,&s) && s.calls==calls+1);
    CHECK(tagpu_recmp_explored_get(c,100,&state,out,sizeof out) && state.lostype==0xFFFF && !memcmp(wanted,out,sizeof out));
    s.tick++; memcpy(s.previous,wanted,sizeof wanted);
    wanted[1]^=1; wanted[40]^=2; wanted[5119]^=4;
    CHECK(tagpu_recmp_explored_encode(c,100,wanted,sizeof wanted,0,emit,&s));
    CHECK(tagpu_recmp_explored_get(c,100,&state,out,sizeof out) && !memcmp(wanted,out,sizeof out));
    CHECK(!tagpu_recmp_explored_get(c,999,&state,out,sizeof out));
    CHECK(!tagpu_recmp_explored_get(c,100,&state,out,sizeof out-1));
    CHECK(!tagpu_recmp_explored_encode(c,100,wanted,sizeof wanted,0x10000,emit,&s));
    CHECK(!tagpu_recmp_explored_encode(c,100,wanted,sizeof wanted-1,0,emit,&s));
    unsigned refusals=0;
    unsigned char packet[18]={0}; RecMpExploreChunk* p=(RecMpExploreChunk*)packet;
    *p=(RecMpExploreChunk){2,5120,2,4,3,{0}}; packet[16]=0x42; packet[17]=0x24;
    RecMpHeader h=header(100,s.sequence+1,RECMP_EXPLORED,s.tick+1,sizeof packet);
    for (unsigned i=0;i<16;i++) { unsigned char old=packet[i]; packet[i]=0xFF;
        if (i==10 || i==11) { packet[i]=old; continue; }
        if (i==0 || i==1) { packet[i]=old; continue; }
        atomic_refusal(c,m,h,packet,sizeof packet); packet[i]=old; refusals++;
    }
    for (unsigned i=0;i<sizeof packet;i++) { RecMpHeader cut=h; cut.bytes=sizeof h+i;
        atomic_refusal(c,m,cut,packet,i); refusals++;
    }
    unsigned char* pages=VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); DWORD old;
    CHECK(pages && VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old));
    for (unsigned i=0;i<sizeof packet;i++) {
        unsigned char* at=pages+4096-i; memcpy(at,packet,i); RecMpHeader cut=h; cut.bytes=sizeof h+i;
        atomic_refusal(c,m,cut,at,i); refusals++;
    }
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
    RecMpHeader bad=h; bad.origin=999; atomic_refusal(c,m,bad,packet,sizeof packet);
    bad=h; bad.stream.Data1++; atomic_refusal(c,m,bad,packet,sizeof packet);
    bad=h; bad.sequence=s.sequence; atomic_refusal(c,m,bad,packet,sizeof packet);
    bad=h; bad.tick=0; atomic_refusal(c,m,bad,packet,sizeof packet);
    bad=h; bad.kind=RECMP_UNITS; atomic_refusal(c,m,bad,packet,sizeof packet);
    for (unsigned i=1;i<10;i++) {
        Sink a={0}; a.cache=c; a.seats=m; a.sequence=1; a.tick=3; a.applied=20; a.body_bytes=sizeof wanted;
        CHECK(tagpu_recmp_explored_encode(c,100+i,wanted,sizeof wanted,i,emit,&a));
        CHECK(tagpu_recmp_explored_get(c,100+i,&state,out,sizeof out) && state.origin==100+i && !memcmp(wanted,out,sizeof out));
    }
    RecMpExplored* partial=tagpu_recmp_explored_begin(&destination,512,320); CHECK(partial);
    Sink f={0}; f.cache=partial; f.seats=m; f.sequence=1; f.tick=3; f.applied=4; f.body_bytes=sizeof wanted; f.fail_call=2;
    CHECK(!tagpu_recmp_explored_encode(partial,100,wanted,sizeof wanted,4,emit,&f));
    CHECK(!tagpu_recmp_explored_idle(partial,100) && !tagpu_recmp_explored_get(partial,100,&state,out,sizeof out));
    CHECK(!tagpu_recmp_explored_encode(partial,100,wanted,sizeof wanted,4,emit,&f));
    atomic_refusal(partial,m,h,packet,sizeof packet);
    tagpu_recmp_explored_end(partial);
    RecMpExplored* tail=tagpu_recmp_explored_begin(&destination,4,12); CHECK(tail);
    unsigned char bits[2]={0xFF,0x0F}; Sink t={0}; t.cache=tail;t.seats=m;t.sequence=1;t.tick=2;t.applied=9;t.body_bytes=2;
    CHECK(tagpu_recmp_explored_encode(tail,100,bits,2,0,emit,&t));
    bits[1]=0xFF; CHECK(!tagpu_recmp_explored_encode(tail,100,bits,2,0,emit,&t));
    tagpu_recmp_explored_end(tail);
    CHECK(!tagpu_recmp_explored_cells(0,4) && !tagpu_recmp_explored_cells(65535,65535) && !tagpu_recmp_explored_cells(3,3));
    tagpu_recmp_explored_end(c); tagpu_recmp_seats_end(m);
    printf("PASS: ten permuted origins, copied input, multipart full/delta atomicity, changed-only/LOS-only, %u guarded/malformed spans, tail bits and failed emission\n",refusals+5);
    return 0;
}
