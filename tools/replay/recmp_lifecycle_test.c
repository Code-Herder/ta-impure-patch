/* Reuse the actual reader's link-boundary stubs; its writer fixture is not run. */
#define RECMP_FILE_TEST_MAIN file_fixture_main
#include "recmp_file_test.c"
#undef RECMP_FILE_TEST_MAIN
#include "tagpu_recmp_lifecycle.h"

typedef struct {
    RecMpLifecycle* life;
    RecMpFileState wire;
    RecMpRoster source, destination;
    unsigned calls, stop_at;
    RecMpLifeAction seen[32];
} LifeFixture;

static int life_visit(void* context, const RecMpLifeAction* a)
{
    LifeFixture* t=context;
    CHECK(t->calls<32);
    t->seen[t->calls++]=*a;
    return !t->stop_at || t->calls!=t->stop_at;
}

static int life_count(void* context, const RecMpLifeAction* a)
{
    unsigned* calls=context;
    CHECK(a->action==RECMP_LIFE_ROOT && a->slot==5 && a->birth==7);
    (*calls)++;
    return 1;
}

static RecMpHeader life_header(const LifeFixture* t, unsigned actor, unsigned kind,
                                unsigned tick, unsigned bytes)
{
    unsigned origin=t->destination.actor[actor].origin;
    RecMpHeader h={ .version=RECMP_VERSION, .header=sizeof h, .bytes=sizeof h+bytes,
                    .origin=origin, .tick=tick, .kind=kind,
                    .sequence=t->wire.source[actor].sequence+1,
                    .match={.Data1=1}, .stream={.Data1=origin} };
    memcpy(h.magic,"IMPUREMP",8);
    return h;
}

static int life_feed(LifeFixture* t, unsigned actor, unsigned kind, unsigned tick,
                       const void* body, unsigned bytes, unsigned proof)
{
    RecMpHeader h=life_header(t,actor,kind,tick,bytes);
    RecMpRecord w={proof ? 0 : t->wire.records+1,proof ? 0 : t->wire.tick+1,
                    proof ? RECMP_RECORD_PROOF : t->wire.header.actor[actor].local ?
                    RECMP_RECORD_PRODUCED : RECMP_RECORD_CONSUMED};
    CHECK(tagpu_recmp_file_record(&t->wire,&w,&h,body,bytes));
    return tagpu_recmp_lifecycle_record(t->life,&w,&h,body,bytes,life_visit,t);
}

static void life_init(LifeFixture* t)
{
    RecMpRecordHeader h={ .version=1,.count=4,.recorder=11,.match={.Data1=1} };
    memset(t,0,sizeof *t);
    t->destination=source_roster(0); t->source=source_roster(1);
    for (unsigned i=0;i<4;i++) h.actor[i]=(RecMpRecordActor){t->destination.actor[i].origin,i<2};
    CHECK(tagpu_recmp_file_state(&t->wire,&h));
    t->life=tagpu_recmp_lifecycle_begin(&h,&t->destination,8); CHECK(t->life);
    for (unsigned i=0;i<4;i++) CHECK(life_feed(t,i,RECMP_SEATS,10,&t->source,sizeof t->source,0));
}

static void life_image(const LifeFixture* t, unsigned actor, unsigned type, void* out)
{
    const RecMpSeat* s=&t->source.actor[actor];
    unsigned owner=s->seat+1, store=t->source.actor[2].seat+1, live=0x10000000u;
    uint16_t utype=type;
    memset(out,0,REC_UNIT_IMAGE);
    memcpy((char*)out+0x96,&owner,4); memcpy((char*)out+0xEC,&store,4);
    memcpy((char*)out+0x110,&live,4); memcpy((char*)out+0xA6,&utype,2);
    ((unsigned char*)out)[0xFF]=s->seat; ((unsigned char*)out)[0xF4]=t->source.actor[1].seat;
}

static unsigned life_units(const LifeFixture* t, unsigned actor, unsigned slot,
                             unsigned live, unsigned birth, unsigned type, unsigned char* body)
{
    RecMpUnitDelta e={slot,live,1,birth,{0},0};
    unsigned count=1;
    memcpy(body,&count,4);
    if (live) { memset(e.bits,255,sizeof e.bits); e.bits[REC_UNIT_BITS-1]=3; }
    memcpy(body+4,&e,sizeof e);
    if (live) life_image(t,actor,type,body+4+sizeof e);
    return 4+sizeof e+(live ? REC_UNIT_IMAGE : 0);
}

static void life_baseline(LifeFixture* t)
{
    unsigned char body[RECMP_BODY_MAX];
    for (unsigned i=0;i<4;i++) {
        unsigned n=life_units(t,i,t->source.actor[i].first,1,7,1,body);
        CHECK(life_feed(t,i,RECMP_UNITS,11,body,n,0));
        n=life_units(t,i,t->source.actor[i].last,0,0,0,body);
        CHECK(life_feed(t,i,RECMP_UNITS,11,body,n,0));
    }
    CHECK(t->calls==8);
}

static unsigned life_root(const LifeFixture* t, unsigned actor, unsigned slot, unsigned birth,
                            unsigned kind, unsigned char* body, unsigned at)
{
    unsigned bytes=kind==RK_EV_REPLY ? 12 : kind==RK_EV_DEATH ? 12+REC_UNIT_IMAGE :
                   kind==RK_EV_DEATH_END ? 4 : 28;
    RecMpRootsEntry e={kind,bytes,1,{0},birth};
    unsigned id=slot+1;
    memcpy(body+at,&e,sizeof e); at+=sizeof e;
    memset(body+at,0,bytes); memcpy(body+at,&id,4);
    if (kind==RK_EV_CREATE || kind==RECMP_ROOT_CREATE_BEGIN) {
        uint16_t type=2;
        memcpy(body+at+4,&type,2); body[at+6]=t->source.actor[actor].seat;
        body[at+7]=3; body[at+8]=1; body[at+24]=kind==RK_EV_CREATE ? 4 : 2;
    } else if (kind==RK_EV_DEATH) {
        body[at+8]=t->source.actor[1].seat;
        life_image(t,actor,2,body+at+12);
    }
    return at+bytes;
}

static unsigned life_cycle(const LifeFixture* t, unsigned char* body)
{
    const unsigned kinds[]={RECMP_ROOT_CREATE_BEGIN,RK_EV_REPLY,RK_EV_CREATE,
                            RK_EV_DEATH,RK_EV_REPLY,RK_EV_DEATH_END};
    unsigned count=6, at=4;
    memcpy(body,&count,4);
    for (unsigned i=0;i<count;i++) at=life_root(t,2,6,8,kinds[i],body,at);
    return at;
}

static void life_controls(void)
{
    LifeFixture t;
    unsigned char body[RECMP_BODY_MAX];
    RecMpLifeState state={0}, sentinel;
    unsigned n;
    life_init(&t); life_baseline(&t);
    n=life_cycle(&t,body); CHECK(life_feed(&t,2,RECMP_ROOTS,20,body,n,0));
    const unsigned expected[]={RECMP_LIFE_CREATE_BEGIN,RECMP_LIFE_ROOT,RECMP_LIFE_CREATE_END,
                               RECMP_LIFE_DEATH_BEGIN,RECMP_LIFE_ROOT,RECMP_LIFE_DEATH_END};
    CHECK(t.calls==14);
    for (unsigned i=0;i<6;i++) CHECK(t.seen[8+i].action==expected[i] &&
        t.seen[8+i].source.tick==20 && t.seen[8+i].where.tick==13 &&
        t.seen[8+i].index==i && t.seen[8+i].birth==8 && t.seen[8+i].slot==6);
    CHECK(t.seen[8].image[6]==2 && t.seen[10].image[24]==4 &&
          t.seen[11].image[8]==1 && t.seen[11].image[12+0xFF]==2 &&
          t.seen[13].bytes==340 && !memcmp(t.seen[11].image,t.seen[13].image,340));
    CHECK(tagpu_recmp_lifecycle_get(t.life,6,&state)==1 && state.phase==RECMP_LIFE_ABSENT && state.birth==8);
    n=life_units(&t,2,6,0,8,0,body); CHECK(life_feed(&t,2,RECMP_UNITS,21,body,n,0));
    for (unsigned i=0;i<4;i++) {
        RecMpSeal seal;
        CHECK(tagpu_recmp_stream_seal(&t.wire.source[i],22,1,&seal));
        CHECK(life_feed(&t,i,RECMP_SEAL,22,&seal,sizeof seal,0));
    }
    RecMpRecordEnd end={.version=1,.count=4,.records=t.wire.records};
    memcpy(end.cut,t.wire.cut,sizeof end.cut);
    CHECK(tagpu_recmp_lifecycle_complete(t.life,&end));
    tagpu_recmp_lifecycle_end(t.life);

    /* A semantically valid final root with a different incarnation prevents
       every earlier action in that batch, including its constructor. */
    life_init(&t); life_baseline(&t); n=life_cycle(&t,body);
    uint32_t wrong=99; memcpy(body+n-4-sizeof(RecMpRootsEntry)+8,&wrong,4);
    CHECK(!life_feed(&t,2,RECMP_ROOTS,20,body,n,0) && t.calls==8);
    memset(&state,0xA5,sizeof state); sentinel=state;
    CHECK(tagpu_recmp_lifecycle_get(t.life,6,&state)==-1 && !memcmp(&state,&sentinel,sizeof state));
    CHECK(!life_feed(&t,2,RECMP_ROOTS,21,body,n,0) && t.calls==8);
    tagpu_recmp_lifecycle_end(t.life);

    for (unsigned bad=0;bad<9;bad++) {
        life_init(&t); life_baseline(&t);
        unsigned count=1, kind=bad==0 ? RK_EV_CREATE : bad==1 ? RK_EV_DEATH_END : RK_EV_REPLY;
        memcpy(body,&count,4);
        n=life_root(&t,2,bad==2 ? 0 : 6,bad==3 ? 7 : 8,kind,body,4);
        if (bad==4 || bad==5 || bad==6) {
            count=2; memcpy(body,&count,4);
            n=life_root(&t,2,6,8,RECMP_ROOT_CREATE_BEGIN,body,4);
            unsigned success=n; n=life_root(&t,2,6,8,RK_EV_CREATE,body,n);
            if (bad==4) body[success+4]=0;
            if (bad==5) body[success+sizeof(RecMpRootsEntry)+8]=2;
            if (bad==6) body[success+sizeof(RecMpRootsEntry)+4]=8;
        }
        if (bad==7 || bad==8) {
            n=life_units(&t,2,5,1,bad==7 ? 8 : 7,bad==8 ? 2 : 1,body);
            CHECK(!life_feed(&t,2,RECMP_UNITS,20,body,n,0) && t.calls==8);
        } else CHECK(!life_feed(&t,2,RECMP_ROOTS,20,body,n,0) && t.calls==8);
        tagpu_recmp_lifecycle_end(t.life);
    }
    life_init(&t); life_baseline(&t); t.stop_at=10; n=life_cycle(&t,body);
    CHECK(!life_feed(&t,2,RECMP_ROOTS,20,body,n,0) && t.calls==10);
    CHECK(!life_feed(&t,2,RECMP_ROOTS,21,body,n,0) && t.calls==10);
    tagpu_recmp_lifecycle_end(t.life);

    life_init(&t);
    n=life_units(&t,2,5,1,7,1,body);
    CHECK(life_feed(&t,2,RECMP_UNITS,11,body,n,1) && !t.calls &&
          !tagpu_recmp_lifecycle_get(t.life,5,&state));
    tagpu_recmp_lifecycle_end(t.life);
    life_init(&t); life_baseline(&t);
    CHECK(tagpu_recmp_lifecycle_get(t.life,5,&state)==1); sentinel=state;
    n=life_units(&t,2,5,1,19,2,body);
    CHECK(life_feed(&t,2,RECMP_UNITS,12,body,n,1) && t.calls==8 &&
          tagpu_recmp_lifecycle_get(t.life,5,&state)==1 && !memcmp(&state,&sentinel,sizeof state));
    tagpu_recmp_lifecycle_end(t.life);
    for (unsigned type=0;type<=8;type+=8) {
        life_init(&t);
        n=life_units(&t,2,5,1,7,type,body);
        CHECK(!life_feed(&t,2,RECMP_UNITS,11,body,n,0) && !t.calls);
        tagpu_recmp_lifecycle_end(t.life);
    }
    life_init(&t); life_baseline(&t);
    unsigned count=167, at=4, calls=0;
    memcpy(body,&count,4);
    for (unsigned i=0;i<count;i++) at=life_root(&t,2,5,7,RK_EV_REPLY,body,at);
    RecMpHeader h=life_header(&t,2,RECMP_ROOTS,12,at);
    RecMpRecord w={t.wire.records+1,t.wire.tick+1,RECMP_RECORD_CONSUMED};
    CHECK(tagpu_recmp_lifecycle_record(t.life,&w,&h,body,at,life_count,&calls) && calls==167);
    tagpu_recmp_lifecycle_end(t.life);
    puts("PASS: normalized owner lifetimes, nested children, constructor water, atomic dependency refusal, permanent callback failure, proof isolation");
}

typedef struct {
    RecMpLifecycle* life;
    const RecHeader* header;
    unsigned char* phase;
    uint64_t count[9], bytes, constructor_children, death_children;
    unsigned crc;
} NativeLife;

static int native_life_action(void* context, const RecMpLifeAction* a)
{
    NativeLife* t=context;
    unsigned fields[]={a->action,a->index,a->slot,a->kind,a->birth,a->before_capture,a->bytes};
    unsigned row=0, old=t->phase[a->slot];
    CHECK(a->action>=1 && a->action<=8 && a->slot>0 && a->slot<t->header->slots);
    for (unsigned i=0;i<t->header->mp_seats.count;i++) {
        const RecMpSeat* s=&t->header->mp_seats.actor[i];
        if (s->origin==a->source.origin && a->slot>=s->first && a->slot<=s->last) { row=s->seat+1; break; }
    }
    CHECK(row);
    const unsigned char* image=a->image;
    if (a->action==RECMP_LIFE_CREATE_BEGIN || a->action==RECMP_LIFE_CREATE_END) CHECK(image[6]==row-1);
    if (a->action==RECMP_LIFE_DEATH_BEGIN || a->action==RECMP_LIFE_DEATH_END) image+=12;
    if (a->action==RECMP_LIFE_BOOTSTRAP || a->action==RECMP_LIFE_IMAGE ||
        a->action==RECMP_LIFE_DEATH_BEGIN || a->action==RECMP_LIFE_DEATH_END) {
        unsigned owner; memcpy(&owner,image+0x96,4); CHECK(owner==row && image[0xFF]==row-1);
    }
    if (a->action==RECMP_LIFE_ROOT) {
        CHECK(old==RECMP_LIFE_CREATING || old==RECMP_LIFE_LIVE || old==RECMP_LIFE_DYING);
        t->constructor_children+=old==RECMP_LIFE_CREATING; t->death_children+=old==RECMP_LIFE_DYING;
    } else if (a->action==RECMP_LIFE_CREATE_BEGIN) t->phase[a->slot]=RECMP_LIFE_CREATING;
    else if (a->action==RECMP_LIFE_DEATH_BEGIN) t->phase[a->slot]=RECMP_LIFE_DYING;
    else t->phase[a->slot]=(a->action==RECMP_LIFE_ABSENCE || a->action==RECMP_LIFE_DEATH_END) ?
                            RECMP_LIFE_ABSENT : RECMP_LIFE_LIVE;
    t->count[a->action]++;
    t->crc=(unsigned)Crc32_ComputeBuf(t->crc,&a->where,sizeof a->where);
    t->crc=(unsigned)Crc32_ComputeBuf(t->crc,&a->source,sizeof a->source);
    t->crc=(unsigned)Crc32_ComputeBuf(t->crc,fields,sizeof fields);
    t->crc=(unsigned)Crc32_ComputeBuf(t->crc,a->image,a->bytes);
    t->bytes+=sizeof a->where+sizeof a->source+sizeof fields+a->bytes;
    return 1;
}

static void native_life_record(void* context, const RecMpRecord* w, const RecMpHeader* h,
                                 const void* body, unsigned bytes)
{
    NativeLife* t=context;
    if (!tagpu_recmp_lifecycle_record(t->life,w,h,body,bytes,native_life_action,t)) {
        fprintf(stderr,"native lifecycle: %s\n",tagpu_recmp_lifecycle_error(t->life)); CHECK(0);
    }
}

static NativeLife native_life_begin(const RecHeader* h)
{
    NativeLife t={ .header=h };
    t.life=tagpu_recmp_lifecycle_begin(&h->mp,&h->mp_seats,h->ndefs);
    t.phase=calloc(h->slots,1); CHECK(t.life && t.phase);
    return t;
}

static void native_life_tail(NativeLife* t, const char* path)
{
    FILE* file=fopen(path,"rb");
    RecMpRecordEnd end;
    unsigned bytes, at=0, crc=t->crc;
    unsigned char* raw;
    CHECK(file && fread(&end,1,sizeof end,file)==sizeof end && fread(&bytes,1,4,file)==4);
    raw=malloc(bytes ? bytes : 1); CHECK(raw && fread(raw,1,bytes,file)==bytes && fgetc(file)==EOF);
    fclose(file);
    while (at<bytes) {
        RecMpRecord w; RecMpHeader h;
        CHECK(bytes-at>=sizeof w+sizeof h);
        memcpy(&w,raw+at,sizeof w); at+=sizeof w; memcpy(&h,raw+at,sizeof h);
        CHECK(w.phase==RECMP_RECORD_PROOF && h.bytes>=sizeof h && h.bytes<=bytes-at);
        native_life_record(t,&w,&h,raw+at+sizeof h,h.bytes-sizeof h); at+=h.bytes;
    }
    CHECK(t->crc==crc && tagpu_recmp_lifecycle_complete(t->life,&end));
    for (unsigned i=0;i<t->header->mp_seats.count;i++) {
        const RecMpSeat* s=&t->header->mp_seats.actor[i];
        for (unsigned slot=s->first;slot<=s->last;slot++) {
            RecMpLifeState state;
            CHECK(tagpu_recmp_lifecycle_get(t->life,slot,&state)==1 &&
                  state.phase==t->phase[slot] && state.origin==s->origin);
        }
    }
    free(raw);
}

static void native_life(const char* path, const char* tail)
{
    char why[256];
    RecReader* r=recr_open(path,why,sizeof why); CHECK(r);
    NativeLife a=native_life_begin(recr_header(r)), b;
    uint64_t ordinal=0;
    unsigned first=0,last=0;
    int k;
    while ((k=recr_tick(r,why,sizeof why))>0) {
        CHECK(k==1);
        if (!first) first=recr_world(r)->tick;
        last=recr_world(r)->tick;
        CHECK(recr_mp_apply(r,&ordinal,native_life_record,&a,why,sizeof why)==1);
        unsigned crc=a.crc;
        CHECK(recr_mp_apply(r,&ordinal,native_life_record,&a,why,sizeof why)==1 && a.crc==crc);
    }
    CHECK(!k && first && last>first);
    native_life_tail(&a,tail);
    CHECK(recr_seek(r,first+(last-first)/2,0,NULL,NULL,why,sizeof why)==1);
    b=native_life_begin(recr_header(r)); ordinal=0;
    do {
        CHECK(recr_mp_apply(r,&ordinal,native_life_record,&b,why,sizeof why)==1);
        k=recr_tick(r,why,sizeof why);
    } while (k>0);
    CHECK(!k); native_life_tail(&b,tail);
    CHECK(a.crc==b.crc && a.bytes==b.bytes && !memcmp(a.count,b.count,sizeof a.count) &&
          a.constructor_children==b.constructor_children && a.death_children==b.death_children &&
          !memcmp(a.phase,b.phase,a.header->slots));
    printf("NATIVE_LIFECYCLE %llu %08X %llu %llu",(unsigned long long)a.bytes,a.crc,
           (unsigned long long)a.constructor_children,(unsigned long long)a.death_children);
    for (unsigned i=1;i<9;i++) printf(" %llu",(unsigned long long)a.count[i]);
    puts("");
    tagpu_recmp_lifecycle_end(a.life); tagpu_recmp_lifecycle_end(b.life);
    free(a.phase); free(b.phase); recr_close(r);
    puts("PASS: native owner lifetime actions, exact source/application order, all owned slots, proof-tail isolation and fresh prefix after world seek; engine application untested");
}

int main(int argc, char** argv)
{
    life_controls();
    if (argc==4 && !strcmp(argv[1],"native")) native_life(argv[2],argv[3]);
    else CHECK(argc==1);
    return 0;
}
