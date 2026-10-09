/* The perspective contributions' codec (tagpu_recmp_pov.c): malformed lengths and enums, provenance,
   chunked selection transactions, a seal while one is partial, and the sampler's room skip. */
#include "tagpu_recmp_pov.h"
#include "tagpu_recmp_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %u: %s\n",__LINE__,#x); exit(1); } } while (0)
#define EACH 1500u

static RecMpHeader header(unsigned origin, unsigned seq, unsigned kind, unsigned tick, unsigned bytes)
{
    RecMpHeader h={0}; memcpy(h.magic,"IMPUREMP",8); h.version=RECMP_VERSION;
    h.header=sizeof h; h.bytes=sizeof h+bytes; h.origin=origin; h.sequence=seq;
    h.tick=tick; h.kind=kind; h.stream.Data1=origin; h.match.Data1=7; return h;
}
/* three actors, origins 100.., each a block of `each` slots */
static RecMpRoster roster(unsigned each)
{
    RecMpRoster r={1,3,each*RECMP_ACTORS+1,each,{{0}}};
    for (unsigned i=0;i<3;i++) r.actor[i]=(RecMpSeat){100+i,i,1+each*i,each*(i+1)};
    return r;
}
static RecMpSeatMaps* maps(RecMpRoster* r)
{
    RecMpSeatMaps* m=tagpu_recmp_seats_begin(r); CHECK(m);
    for (unsigned i=0;i<r->count;i++) {
        RecMpHeader h=header(r->actor[i].origin,1,RECMP_SEATS,1,sizeof *r);
        CHECK(tagpu_recmp_seats_put(m,&h,r,sizeof *r));
    }
    return m;
}
static RecPov pov(int x)
{
    RecPov p={RECMP_POV_PLAYER,RECMP_POV_EVERY,REC_POV_WORLD,0,640,480,65536,x,2*x,x+1,0,x+2};
    return p;
}
static RecClick click(void)
{
    RecClick c={RECMP_POV_PLAYER,REC_CLICK_RIGHT,1,3,2,1,4,10,0,-10};
    return c;
}
/* a selection chunk into b: its bytes */
static unsigned chunk(unsigned char* b, unsigned flags, unsigned total, unsigned first, unsigned count,
                      unsigned slot0)
{
    RecSel s={RECMP_POV_PLAYER,(uint8_t)flags,(uint16_t)total,(uint16_t)first,(uint16_t)count};
    memcpy(b,&s,sizeof s);
    for (unsigned i=0;i<count;i++) {
        RecSelEntry e={(uint16_t)(slot0+i),1000+slot0+i};
        memcpy(b+sizeof s+i*sizeof e,&e,sizeof e);
    }
    return (unsigned)REC_SEL_BYTES(count);
}

/* A refusal leaves every query's answer as it was. */
typedef struct { int pk, ck, sk; RecMpPovState p; RecMpClickState c; RecMpSelectState s; } Snap;
static Snap snap(const RecMpPov* c, unsigned origin)
{
    Snap s; memset(&s,0,sizeof s);
    s.pk=tagpu_recmp_pov_get(c,origin,&s.p); s.ck=tagpu_recmp_pov_click(c,origin,&s.c);
    s.sk=tagpu_recmp_pov_select(c,origin,&s.s,NULL,0);
    return s;
}
static void refused(RecMpPov* c, RecMpSeatMaps* m, RecMpHeader h, const void* body, unsigned bytes)
{
    Snap a=snap(c,h.origin), b;
    int idle=tagpu_recmp_pov_idle(c,h.origin);
    CHECK(!tagpu_recmp_pov_put(c,m,&h,body,bytes,900));
    b=snap(c,h.origin);
    CHECK(!memcmp(&a,&b,sizeof a) && tagpu_recmp_pov_idle(c,h.origin)==idle);
}

typedef struct { RecMpPov* c; RecMpSeatMaps* m; unsigned seq, tick, applied, calls, fail_call, chunks[8]; } Sink;
static int emit(void* ctx, unsigned origin, const void* body, unsigned bytes)
{
    Sink* s=ctx; RecSel head; RecMpSelectState before, after;
    memcpy(&head,body,sizeof head);
    if (++s->calls==s->fail_call) return 0;
    CHECK(s->calls<=8); s->chunks[s->calls-1]=head.count;
    int known=tagpu_recmp_pov_select(s->c,origin,&before,NULL,0);
    RecMpHeader h=header(origin,++s->seq,RECMP_SELECT,s->tick,bytes);
    CHECK(tagpu_recmp_pov_put(s->c,s->m,&h,body,bytes,s->applied));
    CHECK(tagpu_recmp_pov_idle(s->c,origin)==!!(head.flags&REC_SEL_END));
    if (!(head.flags&REC_SEL_END)) {
        /* only END publishes: a partial transaction leaves the last whole selection in force */
        CHECK(tagpu_recmp_pov_select(s->c,origin,&after,NULL,0)==known);
        if (known) CHECK(!memcmp(&before,&after,sizeof before));
    }
    return 1;
}

static void samples(void)
{
    RecMpRoster r=roster(10);
    RecMpSeatMaps* m=maps(&r);
    RecMpPov* c=tagpu_recmp_pov_begin(&r);
    RecMpPovState st;
    RecPov p=pov(5), q;
    RecMpHeader h;
    unsigned char raw[64];
    CHECK(c && !tagpu_recmp_pov_get(c,100,&st));
    /* THE SAMPLER: the first sample needs only room; one queue slot short skips it */
    CHECK(tagpu_recmp_pov_due(c,100,&p,100,RECMP_POV_ROOM)==1);
    CHECK(tagpu_recmp_pov_due(c,100,&p,100,RECMP_POV_ROOM-1)==0);
    CHECK(tagpu_recmp_pov_due(c,999,&p,100,256)==-1);
    h=header(100,2,RECMP_POV,100,sizeof p);
    CHECK(tagpu_recmp_pov_put(c,m,&h,&p,sizeof p,150));
    CHECK(tagpu_recmp_pov_get(c,100,&st) && st.count==1 && st.sample[0].source_tick==100 &&
          st.sample[0].applied_tick==150 && st.sample[0].sequence==2 && !memcmp(&st.sample[0].pov,&p,sizeof p));
    /* fewer than `every` ticks after the last, or unchanged: skipped */
    q=pov(6);
    CHECK(tagpu_recmp_pov_due(c,100,&q,100+RECMP_POV_EVERY-1,256)==0);
    CHECK(tagpu_recmp_pov_due(c,100,&p,100+RECMP_POV_EVERY,256)==0);
    CHECK(tagpu_recmp_pov_due(c,100,&q,100+RECMP_POV_EVERY,256)==1);
    /* A ROOM-SKIPPED SAMPLE: a changed sample with the queue short is not sent, the last sample
       stays the one sent, and the next tick with room sends the change */
    CHECK(tagpu_recmp_pov_due(c,100,&q,100+RECMP_POV_EVERY,RECMP_POV_ROOM-1)==0);
    CHECK(tagpu_recmp_pov_get(c,100,&st) && st.count==1 && !memcmp(&st.sample[0].pov,&p,sizeof p));
    CHECK(tagpu_recmp_pov_due(c,100,&q,100+RECMP_POV_EVERY+1,RECMP_POV_ROOM)==1);
    h=header(100,3,RECMP_POV,107,sizeof q);
    CHECK(tagpu_recmp_pov_put(c,m,&h,&q,sizeof q,160));
    q=pov(7); h=header(100,4,RECMP_POV,113,sizeof q);
    CHECK(tagpu_recmp_pov_put(c,m,&h,&q,sizeof q,160));
    CHECK(tagpu_recmp_pov_get(c,100,&st) && st.count==2 && st.sample[0].source_tick==107 &&
          st.sample[1].source_tick==113 && st.sample[1].pov.cam_x==7 && st.sample[0].pov.cam_x==6);
    /* malformed lengths and fields */
    memset(raw,0,sizeof raw); memcpy(raw,&q,sizeof q);
    refused(c,m,header(100,5,RECMP_POV,120,sizeof q-1),raw,sizeof q-1);
    refused(c,m,header(100,5,RECMP_POV,120,sizeof q+1),raw,sizeof q+1);
    {
        static const struct { unsigned off, size; uint32_t value; } bad[]={
            {0,1,0},{0,1,9},{1,1,0},{1,1,REC_POV_EVERY_MAX+1},{2,1,4},{3,1,1},
            {4,2,0},{4,2,REC_POV_VIEW_MAX+1},{6,2,0},{6,2,REC_POV_VIEW_MAX+1},
            {8,4,REC_POV_ZOOM_MIN-1},{8,4,REC_POV_ZOOM_MAX+1}};
        for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
            RecPov b=pov(8);
            memcpy((unsigned char*)&b+bad[i].off,&bad[i].value,bad[i].size);
            refused(c,m,header(100,5,RECMP_POV,120,sizeof b),&b,sizeof b);
        }
    }
    /* provenance: a repeated sequence, either clock back, another stream, an origin outside
       the roster or without its source roster, another kind */
    q=pov(9);
    refused(c,m,header(100,4,RECMP_POV,120,sizeof q),&q,sizeof q);
    refused(c,m,header(100,5,RECMP_POV,112,sizeof q),&q,sizeof q);
    h=header(100,5,RECMP_POV,120,sizeof q);
    CHECK(!tagpu_recmp_pov_put(c,m,&h,&q,sizeof q,159));
    h.stream.Data1=999; refused(c,m,h,&q,sizeof q);
    h=header(999,5,RECMP_POV,120,sizeof q); CHECK(!tagpu_recmp_pov_put(c,m,&h,&q,sizeof q,160));
    h=header(100,5,RECMP_EXPLORED,120,sizeof q); refused(c,m,h,&q,sizeof q);
    h=header(100,5,RECMP_POV,120,sizeof q); h.bytes++; refused(c,m,h,&q,sizeof q);
    {
        RecMpSeatMaps* none=tagpu_recmp_seats_begin(&r);
        h=header(101,2,RECMP_POV,120,sizeof q);
        CHECK(none && !tagpu_recmp_pov_put(c,none,&h,&q,sizeof q,160) && !tagpu_recmp_pov_get(c,101,&st));
        tagpu_recmp_seats_end(none);
    }
    h=header(100,5,RECMP_POV,120,sizeof q);
    CHECK(tagpu_recmp_pov_put(c,m,&h,&q,sizeof q,160));
    /* clicks */
    {
        RecClick k=click(), b;
        RecMpClickState cs;
        CHECK(!tagpu_recmp_pov_click(c,100,&cs));
        h=header(100,6,RECMP_CLICK,120,sizeof k);
        CHECK(tagpu_recmp_pov_put(c,m,&h,&k,sizeof k,161));
        CHECK(tagpu_recmp_pov_click(c,100,&cs) && cs.source_tick==120 && cs.applied_tick==161 &&
              !memcmp(&cs.click,&k,sizeof k));
        memset(raw,0,sizeof raw); memcpy(raw,&k,sizeof k);
        refused(c,m,header(100,7,RECMP_CLICK,121,sizeof k-1),raw,sizeof k-1);
        refused(c,m,header(100,7,RECMP_CLICK,121,sizeof k+1),raw,sizeof k+1);
        b=k; b.player=2;  refused(c,m,header(100,7,RECMP_CLICK,121,sizeof b),&b,sizeof b);
        b=k; b.button=2;  refused(c,m,header(100,7,RECMP_CLICK,121,sizeof b),&b,sizeof b);
        b=k; b.region=4;  refused(c,m,header(100,7,RECMP_CLICK,121,sizeof b),&b,sizeof b);
        b=k; b.itype=2;   refused(c,m,header(100,7,RECMP_CLICK,121,sizeof b),&b,sizeof b);
        b=k; b.keys=0x10; refused(c,m,header(100,7,RECMP_CLICK,121,sizeof b),&b,sizeof b);
        refused(c,m,header(100,6,RECMP_CLICK,121,sizeof k),&k,sizeof k);
    }
    tagpu_recmp_pov_end(c); tagpu_recmp_seats_end(m);
    puts("PASS: camera samples and clicks: lengths, fields, provenance, two held, room-skipped sample");
}

static void selections(void)
{
    RecMpRoster r=roster(EACH);
    RecMpSeatMaps* m=maps(&r);
    RecMpPov* c=tagpu_recmp_pov_begin(&r);
    static unsigned char b[RECMP_BODY_MAX+16];
    static RecSelEntry in[EACH], out[EACH];
    RecMpSelectState st;
    unsigned n, seq=2;
    CHECK(c && RECMP_SELECT_CHUNK==670);
    /* one whole chunk: the actor 101's block is 1501..3000 */
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,3,0,3,1501);
    RecMpHeader h=header(101,seq++,RECMP_SELECT,100,n);
    CHECK(tagpu_recmp_pov_put(c,m,&h,b,n,200) && tagpu_recmp_pov_idle(c,101));
    CHECK(tagpu_recmp_pov_select(c,101,&st,out,3) && st.count==3 && out[0].slot==1501 &&
          out[2].birth==1000+1503 && st.source_tick==100 && st.applied_tick==200);
    CHECK(!tagpu_recmp_pov_select(c,101,&st,out,2));
    /* an empty selection is one empty chunk, BEGIN|END */
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,0,0,0,0); h=header(101,seq++,RECMP_SELECT,101,n);
    CHECK(tagpu_recmp_pov_put(c,m,&h,b,n,201) && tagpu_recmp_pov_select(c,101,&st,out,0) && !st.count);
    /* malformed lengths, enums and bounds, each refused whole */
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,2,0,2,1501);
    refused(c,m,header(101,seq,RECMP_SELECT,102,n-1),b,n-1);
    refused(c,m,header(101,seq,RECMP_SELECT,102,7),b,7);
    b[n]=0; refused(c,m,header(101,seq,RECMP_SELECT,102,n+1),b,n+1);
    b[1]=7; refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    b[1]=REC_SEL_BEGIN|REC_SEL_END; b[0]=1; refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,0,0,0,0); b[2]=1;          /* empty chunk, total 1 */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN,0,0,0,0);                               /* empty chunk without END */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,EACH+1,0,1,1501);           /* total past the block */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,1,0,2,1501);                /* past the total */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,2,0,1,1501);                /* END short of the total */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN,1,0,1,1501);                            /* not END, total reached */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,1,0,1,1500);                /* below the block */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,1,0,1,3001);                /* above it */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,2,0,2,1501);
    memcpy(b+sizeof(RecSel)+6,b+sizeof(RecSel),2);                  /* a slot twice */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,2,0,2,1501);
    { uint16_t s=1500+3; memcpy(b+sizeof(RecSel),&s,2); }           /* descending */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    n=chunk(b,REC_SEL_END,1,0,1,1501);                              /* a continuation, nothing staged */
    refused(c,m,header(101,seq,RECMP_SELECT,102,n),b,n);
    {
        RecMpRoster small=roster(10);
        RecMpSeatMaps* sm=maps(&small);
        RecMpPov* sc=tagpu_recmp_pov_begin(&small);
        CHECK(sc);
        n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,11,0,1,11);             /* the units a player may have */
        refused(sc,sm,header(101,2,RECMP_SELECT,102,n),b,n);
        n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,10,0,10,11);
        h=header(101,2,RECMP_SELECT,102,n); CHECK(tagpu_recmp_pov_put(sc,sm,&h,b,n,1));
        tagpu_recmp_pov_end(sc); tagpu_recmp_seats_end(sm);
    }
    /* CHUNKED TRANSACTIONS: a whole block, 1500 entries, goes as 670 + 670 + 160 */
    for (unsigned i=0;i<EACH;i++) in[i]=(RecSelEntry){(uint16_t)(1501+i),7000+i};
    {
        Sink s={c,m,seq-1,103,300,0,0,{0}};
        CHECK(tagpu_recmp_select_encode(c,101,in,EACH,emit,&s));
        CHECK(s.calls==3 && s.chunks[0]==670 && s.chunks[1]==670 && s.chunks[2]==160);
        CHECK(tagpu_recmp_pov_select(c,101,&st,out,EACH) && st.count==EACH && st.sequence==s.seq &&
              !memcmp(in,out,sizeof in));
        seq=s.seq+1;
        /* a partial transaction: the emission fails after the first chunk */
        Sink f={c,m,seq-1,104,301,0,2,{0}};
        CHECK(!tagpu_recmp_select_encode(c,101,in,800,emit,&f));
        CHECK(!tagpu_recmp_pov_idle(c,101) && tagpu_recmp_pov_select(c,101,&st,out,EACH) &&
              st.count==EACH && !memcmp(in,out,sizeof in));
        CHECK(!tagpu_recmp_select_encode(c,101,in,10,emit,&f));     /* nothing over a partial one */
        seq=f.seq+1;
    }
    /* continuation discontinuities against the staged 670 */
    n=chunk(b,REC_SEL_END,800,670,130,1501+670);
    refused(c,m,header(101,seq+1,RECMP_SELECT,104,n),b,n);           /* a sequence skipped */
    refused(c,m,header(101,seq,RECMP_SELECT,105,n),b,n);             /* another source tick */
    h=header(101,seq,RECMP_SELECT,104,n); CHECK(!tagpu_recmp_pov_put(c,m,&h,b,n,300)); /* applied back */
    n=chunk(b,REC_SEL_END,801,670,131,1501+670);                    /* the total changed */
    refused(c,m,header(101,seq,RECMP_SELECT,104,n),b,n);
    n=chunk(b,REC_SEL_END,800,671,129,1501+671);                    /* not where the last ended */
    refused(c,m,header(101,seq,RECMP_SELECT,104,n),b,n);
    n=chunk(b,REC_SEL_END,800,670,130,1501+669);                    /* not above the last slot */
    refused(c,m,header(101,seq,RECMP_SELECT,104,n),b,n);
    n=chunk(b,REC_SEL_BEGIN|REC_SEL_END,1,0,1,1501);                /* an interrupted transaction */
    refused(c,m,header(101,seq,RECMP_SELECT,104,n),b,n);
    n=chunk(b,REC_SEL_END,800,670,130,1501+670);
    h=header(101,seq,RECMP_SELECT,104,n);
    CHECK(tagpu_recmp_pov_put(c,m,&h,b,n,302) && tagpu_recmp_pov_idle(c,101));
    CHECK(tagpu_recmp_pov_select(c,101,&st,out,EACH) && st.count==800 && out[799].slot==1501+799);
    /* encode refuses a selection that put would refuse, before emitting anything */
    {
        Sink s={c,m,seq,105,303,0,0,{0}};
        in[0].slot=1500; CHECK(!tagpu_recmp_select_encode(c,101,in,2,emit,&s)); in[0].slot=1501;
        in[1].slot=1501; CHECK(!tagpu_recmp_select_encode(c,101,in,2,emit,&s)); in[1].slot=1502;
        CHECK(!tagpu_recmp_select_encode(c,101,in,EACH+1,emit,&s));
        CHECK(!tagpu_recmp_select_encode(c,999,in,2,emit,&s));
        CHECK(!s.calls);
        CHECK(tagpu_recmp_select_encode(c,101,in,0,emit,&s) && s.calls==1 &&
              tagpu_recmp_pov_select(c,101,&st,NULL,0) && !st.count);
    }
    tagpu_recmp_pov_end(c); tagpu_recmp_seats_end(m);
    puts("PASS: selection chunks: lengths, enums, block bounds, order, 670-entry transactions, partial and interrupted");
}

/* A SEAL WHILE PARTIAL: the history refuses a source seal over a partial selection transaction,
   as it does over a partial order or exploration transaction; the same seal after END passes. */
static int sealed(int partial)
{
    RecMpRecordHeader file={1,2,100,0,{.Data1=7},{{100,1},{101,0}}};
    RecMpRoster r=roster(EACH);
    RecMpHistory* history;
    RecMpStream stream={0};
    RecMpSeal seal;
    unsigned char b[RECMP_BODY_MAX];
    unsigned n, ok, k;
    r.count=2; memset(&r.actor[2],0,sizeof r.actor[2]);
    history=tagpu_recmp_history_begin(&file,&r,4,4); CHECK(history);
    struct { unsigned kind, bytes; const void* body; } rec[3];
    RecMpHeader h;
    n=chunk(b,partial ? REC_SEL_BEGIN : REC_SEL_BEGIN|REC_SEL_END,partial ? 2 : 1,0,1,1501);
    rec[0].kind=RECMP_SEATS; rec[0].bytes=sizeof r; rec[0].body=&r;
    rec[1].kind=RECMP_SELECT; rec[1].bytes=n; rec[1].body=b;
    for (k=0;k<2;k++) {
        RecMpRecord where={k+1,50,RECMP_RECORD_CONSUMED};
        h=header(101,k+1,rec[k].kind,10,rec[k].bytes);
        CHECK(tagpu_recmp_stream_put(&stream,&h,rec[k].body,rec[k].bytes));
        CHECK(tagpu_recmp_history_record(history,&where,&h,rec[k].body,rec[k].bytes));
    }
    CHECK(tagpu_recmp_stream_seal(&stream,10,0,&seal));
    {
        RecMpRecord where={3,51,RECMP_RECORD_CONSUMED};
        h=header(101,3,RECMP_SEAL,10,sizeof seal);
        ok=tagpu_recmp_history_record(history,&where,&h,&seal,sizeof seal);
    }
    if (ok) {
        RecMpSelectState st;
        RecSelEntry e;
        CHECK(tagpu_recmp_history_select(history,101,&st,&e,1) && st.count==1 && e.slot==1501 &&
              st.applied_tick==50);
    }
    tagpu_recmp_history_end(history);
    return (int)ok;
}

int main(void)
{
    samples();
    selections();
    CHECK(sealed(0) && !sealed(1));
    puts("PASS: a seal closes a whole selection transaction and refuses a partial one");
    return 0;
}
