/* Actual writer/reader fixture using the controlled native channel boundaries. */
static void file_proof_tail(void);
#define RECMP_FILE_HISTORY
#define main cut_test_main
#include "recmp_cut_test.c"
#undef main
#include "tagpu_recorder.h"
#include "tagpu_recreader.h"
#include "tagpu_recmp_history.h"
#include "crc32.h"

static RecMpRoster source_roster(unsigned remote)
{
    RecMpRoster r={ .version=1, .count=4, .slots=21, .each=2 };
    const unsigned origin[]={11,22,55,66}, seat[]={7,4,0,2};
    for (unsigned i=0;i<4;i++) r.actor[i]=(RecMpSeat){origin[i],remote ? seat[i] : i,1+2*i,2+2*i};
    return r;
}

static unsigned player_body(unsigned origin, unsigned full, unsigned char* out)
{
    RecMpPlayerDelta delta={0};
    unsigned rank=origin==11 ? 0 : origin==22 ? 1 : origin==55 ? 2 : 3;
    unsigned image[REC_PLAYER_DWORDS], first=2+2*rank, last=3+2*rank;
    delta.origin=origin; delta.controller=1+(rank&1); delta.full=full;
    memset(delta.bits,full ? 255 : 0,sizeof delta.bits);
    delta.bits[REC_PLAYER_BITS-1]=full ? 3 : 0;
    if (!full) delta.bits[0]=1;
    memcpy(out,&delta,sizeof delta);
    for (unsigned i=0;i<REC_PLAYER_DWORDS;i++) image[i]=origin;
    memcpy((unsigned char*)image+0x67,&first,4); memcpy((unsigned char*)image+0x6B,&last,4);
    ((unsigned char*)image)[0x146]=source_roster(rank>=2).actor[rank].seat;
    for (unsigned v=0;v<5;v++) for (unsigned row=0;row<11;row++)
        ((unsigned char*)image)[0x108+11*v+row]=(unsigned char)(20*v+row+1);
    if (!full) image[0]++;
    memcpy(out+sizeof delta,image,full ? sizeof image : 4);
    return sizeof delta+(full ? sizeof image : 4);
}

static void source_player(unsigned origin, unsigned sequence, unsigned tick, unsigned full)
{
    unsigned char body[sizeof(RecMpPlayerDelta)+REC_PLAYER_IMAGE];
    unsigned n=player_body(origin,full,body);
    RecMpPacket* p=packet(origin,RECMP_PLAYER,sequence,tick,body,n);
    CHECK(tagpu_recmp_stream_put(&f.source[origin==66],&p->h,p->body,n));
}

static unsigned unit_body(unsigned origin, unsigned full, unsigned char* out)
{
    unsigned rank=origin==11 ? 0 : origin==22 ? 1 : origin==55 ? 2 : 3;
    RecMpRoster source=source_roster(rank>=2);
    RecMpUnitDelta entry={ .slot=1+2*rank, .live=1, .full=full, .birth=rank==3 ? 17 : 23 };
    unsigned image[REC_UNIT_DWORDS], count=full ? 2 : 1, value;
    memset(entry.bits,full ? 255 : 0,sizeof entry.bits); entry.bits[REC_UNIT_BITS-1]=full ? 3 : 0;
    if (!full) entry.bits[26>>3]=1u<<(26&7);
    for (unsigned i=0;i<REC_UNIT_DWORDS;i++) image[i]=origin;
    value=source.actor[rank].seat+1;
    memcpy((unsigned char*)image+0x96,&value,4); memcpy((unsigned char*)image+0xEC,&value,4);
    ((unsigned char*)image)[0xFF]=source.actor[rank].seat;
    ((unsigned char*)image)[0xF4]=source.actor[rank==3 ? 2 : 0].seat;
    image[0x110/4]=0x10000000u;
    memcpy(out,&count,4); memcpy(out+4,&entry,sizeof entry);
    if (!full) { value=origin+1; memcpy(out+4+sizeof entry,&value,4); return 4+sizeof entry+4; }
    memcpy(out+4+sizeof entry,image,sizeof image);
    memset(&entry,0,sizeof entry); entry.slot=2+2*rank; entry.full=1;
    memcpy(out+4+sizeof entry+sizeof image,&entry,sizeof entry);
    return 4+2*sizeof entry+sizeof image;
}

static void source_units(unsigned sequence, unsigned tick, unsigned full)
{
    unsigned char body[RECMP_BODY_MAX];
    unsigned n=unit_body(66,full,body);
    RecMpPacket* p=packet(66,RECMP_UNITS,sequence,tick,body,n);
    CHECK(tagpu_recmp_stream_put(&f.source[1],&p->h,p->body,n));
}

static unsigned named_body(unsigned origin, unsigned proof, unsigned char* body)
{
    unsigned count=2, at=4, id=origin==11 ? 2 : origin==22 ? 4 : 6;
    const char* names[]={"TakeDamage","TargetCleared"};
    memcpy(body,&count,4);
    for (unsigned i=0;i<count;i++) {
        unsigned n=(unsigned)strlen(names[i]), arg=origin+proof+i;
        RecMpNamedEntry e={24+n,i,0,17};
        memcpy(body+at,&e,sizeof e); at+=sizeof e;
        memcpy(body+at,&id,4); body[at+4]=1; body[at+5]=i;
        body[at+6]=body[at+7]=0; memset(body+at+8,0,16); memcpy(body+at+8,&arg,4);
        memcpy(body+at+24,names[i],n); at+=24+n;
    }
    return at;
}

static void source_named(unsigned sequence, unsigned proof)
{
    unsigned char body[RECMP_BODY_MAX];
    unsigned n=named_body(55,proof,body);
    RecMpPacket* p=packet(55,RECMP_NAMED,sequence,8,body,n);
    CHECK(tagpu_recmp_stream_put(&f.source[0],&p->h,p->body,n));
}

static unsigned roots_body(unsigned origin, unsigned proof, unsigned char* body)
{
    const unsigned kinds[]={RK_EV_START_NAMED,RK_EV_REPLY,RK_EV_SPRAY,RK_EV_CREATE,
                            RK_EV_DEATH,RK_EV_DEATH_END,RECMP_ROOT_CREATE_BEGIN};
    const unsigned spans[]={28,14,48,28,340,4,28};
    unsigned count=7, at=4, rank=origin==11 ? 0 : origin==22 ? 1 : 2, id=2+2*rank;
    memcpy(body,&count,4);
    for (unsigned i=0;i<count;i++) {
        RecMpRootsEntry e={kinds[i],spans[i],i&1,{0},17};
        unsigned value;
        memcpy(body+at,&e,sizeof e); at+=sizeof e;
        memset(body+at,0,spans[i]); memcpy(body+at,&id,4);
        if (!i) {
            body[at+4]=1; value=origin+proof; memcpy(body+at+8,&value,4);
            memcpy(body+at+24,"Move",4);
        } else if (i==1) {
            value=23; memcpy(body+at+4,&value,4); body[at+8]=body[at+9]=1;
            memcpy(body+at+12,proof ? "NO" : "OK",2);
        } else if (i==2) {
            value=1; memcpy(body+at+4,&value,4);
            value=origin+proof; memcpy(body+at+8,&value,4);
            memset(body+at+12,0xA5,32); value=0xABCD0004; memcpy(body+at+44,&value,4);
        } else if (kinds[i]==RK_EV_CREATE || kinds[i]==RECMP_ROOT_CREATE_BEGIN) {
            uint16_t type=1;
            memcpy(body+at+4,&type,2); body[at+6]=source_roster(rank>=2).actor[rank].seat;
            body[at+7]=3; value=0x80000001; memcpy(body+at+8,&value,4);
            value=origin+proof; memcpy(body+at+12,&value,4);
            value=0x80000000; memcpy(body+at+16,&value,4);
            value=0x7FFFFFFF; memcpy(body+at+20,&value,4); body[at+24]=4;
        } else if (i==4) {
            unsigned row=source_roster(rank>=2).actor[rank].seat;
            value=2; memcpy(body+at+4,&value,4); body[at+8]=10;
            body[at+9]=proof; body[at+10]=proof+5; body[at+11]=proof&1;
            memset(body+at+12,0xA5,REC_UNIT_IMAGE);
            value=row+1; memcpy(body+at+12+0x96,&value,4);
            value=11; memcpy(body+at+12+0xEC,&value,4);
            body[at+12+0xF4]=10; body[at+12+0xFF]=row;
            value=0x10000000u|(origin+proof); memcpy(body+at+12+0x110,&value,4);
        }
        at+=spans[i];
    }
    return at;
}

static void source_roots(unsigned sequence, unsigned proof)
{
    unsigned char body[RECMP_BODY_MAX];
    unsigned n=roots_body(55,proof,body);
    RecMpPacket* p=packet(55,RECMP_ROOTS,sequence,8,body,n);
    CHECK(tagpu_recmp_stream_put(&f.source[0],&p->h,p->body,n));
}

static void source_orders(unsigned origin, unsigned sequence, unsigned tick, unsigned total,
                           unsigned first, unsigned count)
{
    RecMpOrders chunk={(uint16_t)(origin==55 ? 5 : 7),0,0,17,
                       (uint16_t)total,(uint16_t)first,(uint16_t)count,0};
    unsigned char body[RECMP_BODY_MAX];
    RecMpPacket* p;
    memcpy(body,&chunk,sizeof chunk);
    for (unsigned i=0;i<count*RECMP_ORDER_BYTES;i++) body[sizeof chunk+i]=(unsigned char)(i+first);
    p=packet(origin,RECMP_ORDERS,sequence,tick,body,sizeof chunk+count*RECMP_ORDER_BYTES);
    CHECK(tagpu_recmp_stream_put(&f.source[origin==66],&p->h,p->body,p->h.bytes-sizeof p->h));
}

static void proof_only_roster(void)
{
    RecMpRecordHeader file={ .version=1, .count=4, .recorder=11, .match={1} };
    RecMpRoster recorder=source_roster(0), source=source_roster(1);
    RecMpHistory* history;
    RecMpRecord where={ .phase=RECMP_RECORD_PROOF };
    RecMpHeader h={ .version=RECMP_VERSION, .header=sizeof h, .origin=55, .tick=8,
                   .sequence=1, .kind=RECMP_SEATS, .match={1}, .stream={2} };
    RecMpPlayerState state;
    RecMpUnitState unit;
    RecMpNamedState named;
    RecMpRootsState roots;
    RecMpExploreState explored;
    unsigned char body[RECMP_BODY_MAX];
    unsigned bytes;
    memcpy(h.magic,"IMPUREMP",8);
    for (unsigned i=0;i<4;i++) file.actor[i]=(RecMpRecordActor){recorder.actor[i].origin,i<2};
    history=tagpu_recmp_history_begin(&file,&recorder,2,2); CHECK(history);
    h.bytes=sizeof h+sizeof source;
    CHECK(tagpu_recmp_history_record(history,&where,&h,&source,sizeof source));
    h.kind=RECMP_PLAYER; h.sequence=2; bytes=player_body(55,1,body); h.bytes=sizeof h+bytes;
    CHECK(tagpu_recmp_history_record(history,&where,&h,body,bytes));
    h.kind=RECMP_UNITS; h.sequence=3; bytes=unit_body(55,1,body); h.bytes=sizeof h+bytes;
    CHECK(tagpu_recmp_history_record(history,&where,&h,body,bytes));
    h.kind=RECMP_NAMED; h.sequence=4; bytes=named_body(55,0,body); h.bytes=sizeof h+bytes;
    CHECK(tagpu_recmp_history_record(history,&where,&h,body,bytes));
    memset(&named,0xA5,sizeof named); CHECK(!tagpu_recmp_history_named(history,55,&named));
    for (unsigned i=0;i<sizeof named;i++) CHECK(((unsigned char*)&named)[i]==0xA5);
    h.kind=RECMP_ROOTS; h.sequence=5; bytes=roots_body(55,0,body); h.bytes=sizeof h+bytes;
    CHECK(tagpu_recmp_history_record(history,&where,&h,body,bytes));
    memset(&roots,0xA5,sizeof roots); CHECK(!tagpu_recmp_history_roots(history,55,&roots));
    for (unsigned i=0;i<sizeof roots;i++) CHECK(((unsigned char*)&roots)[i]==0xA5);
    {
        RecMpExploreChunk chunk={0,1,1,5,7,{0}};
        memcpy(body,&chunk,sizeof chunk); body[sizeof chunk]=1;
        h.kind=RECMP_EXPLORED; h.sequence=6; h.bytes=sizeof h+sizeof chunk+1;
        CHECK(tagpu_recmp_history_record(history,&where,&h,body,sizeof chunk+1));
        memset(&explored,0xA5,sizeof explored);
        CHECK(!tagpu_recmp_history_explored(history,55,&explored,body,sizeof body));
        for (unsigned i=0;i<sizeof explored;i++) CHECK(((unsigned char*)&explored)[i]==0xA5);
    }
    memset(&state,0xA5,sizeof state);
    CHECK(!tagpu_recmp_history_player(history,55,&state));
    for (unsigned i=0;i<sizeof state;i++) CHECK(((unsigned char*)&state)[i]==0xA5);
    memset(&unit,0xA5,sizeof unit); CHECK(!tagpu_recmp_history_unit(history,5,&unit));
    for (unsigned i=0;i<sizeof unit;i++) CHECK(((unsigned char*)&unit)[i]==0xA5);
    tagpu_recmp_history_end(history);
    puts("PASS: proof-only source roster/player validate without publishing application authority");
}

static void file_proof_tail(void)
{
    for (unsigned i=0;i<2;i++) {
        RecMpSeal seal;
        RecMpPacket* p;
        if (f.missing_cut && i==1) continue;
        if (!i) {
            RecMpExploreChunk chunk={0,1,1,0xFFFF,7,{0}};
            unsigned char body[sizeof chunk+1];
            source_named(6,1); source_roots(7,1);
            memcpy(body,&chunk,sizeof chunk); body[sizeof chunk]=1;
            p=packet(55,RECMP_EXPLORED,8,8,body,sizeof body);
            CHECK(tagpu_recmp_stream_put(&f.source[0],&p->h,p->body,sizeof body));
        }
        if (i==1) {
            source_orders(66,6,9,46,45,1);
            source_player(66,7,10,0);
            source_units(8,10,0);
        }
        CHECK(tagpu_recmp_stream_seal(&f.source[i],i ? 10 : 8,0,&seal));
        p=packet(i ? 66 : 55,RECMP_SEAL,f.source[i].sequence+1,i ? 10 : 8,&seal,sizeof seal);
        CHECK(tagpu_recmp_stream_put(&f.source[i],&p->h,p->body,sizeof seal));
    }
}

static unsigned append(unsigned char* b, unsigned at, unsigned kind, const void* body, unsigned bytes)
{
    RecHdr h={(uint16_t)kind,(uint16_t)bytes};
    memcpy(b+at,&h,sizeof h);
    if (bytes) memcpy(b+at+4,body,bytes);
    memset(b+at+4+bytes,0,((bytes+3)&~3u)-bytes);
    return at+4+((bytes+3)&~3u);
}

static unsigned tlv32(unsigned char* b, unsigned at, unsigned tag, unsigned value)
{
    return append(b,at,tag,&value,4);
}

static HANDLE world_start(RecMpFile* mp, const char* path, RecRing** ring, unsigned* write)
{
    RecRing* g=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *g);
    unsigned char header[2048], player[4+REC_PLAYER_IMAGE]={0}, seat[16+REC_INFO_BYTES]={0};
    unsigned globals[REC_GLOBALS_DWORDS]={0}, proj[2]={0}, mapped[2]={0};
    unsigned features[5]={2,2,1,0,0}, defs[2]={8,0}, tick=100, at=0, n=0;
    RecNetCreate create={6,1,2,4,0x80000011u,{123456,-654321,123},0.25f,65535,0};
    unsigned short grid[2]={2,2};
    HANDLE done;
    CHECK(g);
    g->buf=VirtualAlloc(NULL,REC_RING_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g->grid=VirtualAlloc(NULL,4*REC_CELL_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g->wake=CreateEvent(NULL,FALSE,FALSE,NULL); g->done=CreateEvent(NULL,TRUE,FALSE,NULL);
    g->mp=mp; snprintf(g->path,sizeof g->path,"%s",path);
    CHECK(g->buf && g->grid && g->wake && g->done && DuplicateHandle(GetCurrentProcess(),g->done,
        GetCurrentProcess(),&done,0,FALSE,DUPLICATE_SAME_ACCESS));
    n=tlv32(header,n,RT_FORMAT,REC_FORMAT); n=tlv32(header,n,RT_MODE,3);
    n=tlv32(header,n,RT_SLOTS,21); n=tlv32(header,n,RT_UNITS_EACH,2); n=tlv32(header,n,RT_START,100);
    n=tlv32(header,n,RT_PROJECTILES,1);
    n=append(header,n,RT_UNITDEFS,defs,sizeof defs);
    n=append(header,n,RT_FEATURES,features,sizeof features);
    n=append(header,n,RT_MULTIPLAYER,tagpu_recmp_file_header(mp),sizeof(RecMpRecordHeader));
    n=append(header,n,RT_MP_SEATS,tagpu_recmp_file_roster(mp),sizeof(RecMpRoster));
    for (unsigned i=0;i<4;i++) {
        seat[0]=i; seat[1]=i<2 ? i+1 : 3;
        n=append(header,n,RT_PLAYER,seat,sizeof seat);
    }
    at=append(g->buf,at,RK_HEADER,header,n);
    at=append(g->buf,at,RK_TICK,&tick,4);
    at=append(g->buf,at,RK_GRID,grid,sizeof grid);
    at=append(g->buf,at,RK_GLOBALS,globals,sizeof globals);
    at=append(g->buf,at,RK_PROJ_HEAD,proj,sizeof proj);
    at=append(g->buf,at,RK_MAPPED,mapped,6);
    player[4]=0xA5;
    at=append(g->buf,at,RK_PLAYER,player,sizeof player);
    at=append(g->buf,at,RK_EV_NET_CREATE,&create,sizeof create);
    tick=101; at=append(g->buf,at,RK_TICK,&tick,4);
    InterlockedExchange(&g->flush_req,1); /* make tick 100 durable before failure controls */
    InterlockedExchange(&g->wpub,at);
    CHECK(tagpu_recwriter_start(g));
    *ring=g; *write=at;
    return done;
}

static void world_end(RecRing* g, unsigned at)
{
    unsigned char end[REC_END_BYTES]={0};
    HANDLE wake;
    end[0]=REC_END_TEARDOWN;
    for (unsigned i=0;i<REC_PLAYERS;i++) end[24+4+12*i+1]=REC_RESULT_NONE;
    CHECK(DuplicateHandle(GetCurrentProcess(),g->wake,GetCurrentProcess(),&wake,0,FALSE,DUPLICATE_SAME_ACCESS));
    at=append(g->buf,at,RK_END,end,sizeof end);
    InterlockedExchange(&g->wend,at); InterlockedExchange(&g->ended,1);
    InterlockedExchange(&g->wpub,at);  /* final producer access; writer can free */
    SetEvent(wake); CloseHandle(wake);
}

static void read_units(RecReader* r, char* why, unsigned cap)
{
    RecMpNamedState named;
    RecMpRootsState roots;
    unsigned char body[RECMP_BODY_MAX];
    for (unsigned i=0;i<3;i++) {
        unsigned origin=i==0 ? 11 : i==1 ? 22 : 55, bytes=named_body(origin,0,body);
        CHECK(recr_mp_named(r,origin,&named,why,cap)==1 && named.count==2 &&
              named.bytes==bytes && named.sequence==4 && named.source_tick==(i<2 ? 100 : 8) &&
              named.applied_tick==100 && !memcmp(named.body,body,bytes));
        unsigned later=recr_world(r)->tick==101 && origin<55;
        unsigned variant=later ? (origin==11 ? 2 : 1) : 0;
        bytes=roots_body(origin,variant,body);
        CHECK(recr_mp_roots(r,origin,&roots,why,cap)==1 && roots.count==7 &&
              roots.bytes==bytes && roots.sequence==(later ? (origin==11 ? 8 : 7) : 5) &&
              roots.source_tick==(later ? 101 : i<2 ? 100 : 8) &&
              roots.applied_tick==(later ? 101 : 100) && !memcmp(roots.body,body,bytes));
    }
    CHECK(recr_mp_named(r,66,&named,why,cap)==0);
    CHECK(recr_mp_roots(r,66,&roots,why,cap)==0);
    RecMpUnitState state;
    RecMpPlayerState player;
    for (unsigned slot=3;slot<=7;slot+=4) {
        unsigned value, row=slot==3 ? 1 : 3, origin=slot==3 ? 22 : 66;
        CHECK(recr_mp_unit(r,slot,&state,why,cap)==1 && state.live && state.origin==origin &&
              state.birth==(slot==3 ? 23 : 17) && state.source_tick==(slot==3 ? 100 : 8) &&
              state.sequence==3 && state.applied_tick==100 && state.image[26]==origin);
        memcpy(&value,(unsigned char*)state.image+0x96,4); CHECK(value==row+1);
        memcpy(&value,(unsigned char*)state.image+0xEC,4); CHECK(value==row+1);
        CHECK(((unsigned char*)state.image)[0xFF]==row && ((unsigned char*)state.image)[0xF4]==(slot==3 ? 0 : 2));
        CHECK(recr_mp_player(r,origin,&player,why,cap)==1 && !memcmp(&state.stream,&player.stream,sizeof state.stream));
        CHECK(recr_mp_unit(r,slot+1,&state,why,cap)==1 && !state.live && state.birth==0 &&
              state.source_tick==(slot==3 ? 100 : 8) && state.sequence==3 && state.applied_tick==100);
        for (unsigned i=0;i<REC_UNIT_DWORDS;i++) CHECK(!state.image[i]);
    }
    CHECK(recr_mp_unit(r,5,&state,why,cap)==0);
}

typedef struct {
    unsigned count, bytes[40], roots;
    RecMpRecord where[40];
    RecMpPacket packet[40];
} Applied;

static void applied_copy(void* ctx, const RecMpRecord* where, const RecMpHeader* packet,
                          const void* body, unsigned bytes)
{
    Applied* a=ctx;
    unsigned i=a->count++;
    CHECK(i<40 && where->phase!=RECMP_RECORD_PROOF && bytes<=RECMP_BODY_MAX &&
          packet->bytes==sizeof *packet+bytes);
    a->where[i]=*where; a->packet[i].h=*packet; a->bytes[i]=bytes;
    memcpy(a->packet[i].body,body,bytes);
    if (packet->kind==RECMP_ROOTS) a->roots++;
}

static void applied_equal(const Applied* a, unsigned first, const Applied* b)
{
    CHECK(a->count-first==b->count);
    for (unsigned i=0;i<b->count;i++) {
        CHECK(a->bytes[first+i]==b->bytes[i] &&
              !memcmp(&a->where[first+i],&b->where[i],sizeof b->where[i]) &&
              !memcmp(&a->packet[first+i],&b->packet[i],sizeof(RecMpHeader)+b->bytes[i]));
    }
}

static void read_applications(RecReader* r, Applied* a, uint64_t* ordinal, char* why, unsigned cap)
{
    unsigned before=a->count;
    CHECK(recr_mp_apply(r,ordinal,applied_copy,a,why,cap)==1 && *ordinal==a->count);
    CHECK(recr_mp_apply(r,ordinal,applied_copy,a,why,cap)==1 && a->count==*ordinal);
    CHECK(a->count>=before);
}

/* The perspective contributions origins 11 and 22 published at 101: none before it, and the same
   after a seek as after reading forward (the query history is rebuilt at a seek). */
static void read_perspective(RecReader* r, char* why, unsigned cap)
{
    RecMpPovState ps;
    RecMpSelectState ss;
    RecSelEntry e[2];
    if (recr_world(r)->tick<101) {
        CHECK(recr_mp_pov(r,11,&ps,why,cap)==0 && recr_mp_select(r,11,&ss,NULL,0,why,cap)==0 &&
              recr_mp_select(r,22,&ss,NULL,0,why,cap)==0);
        return;
    }
    CHECK(recr_mp_pov(r,11,&ps,why,cap)==1 && ps.count==1 && ps.sample[0].origin==11 &&
          ps.sample[0].source_tick==101 && ps.sample[0].applied_tick==101 &&
          ps.sample[0].pov.cam_x==321 && ps.sample[0].pov.zoom==98304);
    CHECK(recr_mp_pov(r,22,&ps,why,cap)==0 && recr_mp_pov(r,55,&ps,why,cap)==0);
    CHECK(recr_mp_select(r,11,&ss,e,2,why,cap)==1 && ss.count==2 && ss.applied_tick==101 &&
          e[0].slot==1 && e[0].birth==21 && e[1].slot==2 && e[1].birth==22);
    CHECK(recr_mp_select(r,11,&ss,e,1,why,cap)==0);
    CHECK(recr_mp_select(r,22,&ss,e,2,why,cap)==1 && ss.count==1 && e[0].slot==3 && e[0].birth==33);
    CHECK(recr_mp_select(r,66,&ss,NULL,0,why,cap)==0);
}

static int read_file(const char* path, int expected)
{
    char why[256];
    RecReader* r=recr_open(path,why,sizeof why);
    RecSummary summary;
    RecInfo* info=calloc(1,sizeof *info);
    CHECK(info);
    if (expected<0) {
        CHECK(!r && !recr_summary(path,&summary,why,sizeof why) && !recr_info(path,info,why,sizeof why));
        printf("PASS: reader, summary and info refuse malformed contribution proof: %s\n",why);
    } else {
        RecMpPlayerState pstate;
        RecMpOrderState ostate;
        RecMpUnitState ustate;
        RecMpNamedState nstate;
        RecMpRootsState rstate;
        Applied* applied=calloc(1,sizeof *applied);
        Applied* replayed=calloc(1,sizeof *replayed);
        uint64_t ordinal=0, replay_ordinal=0;
        unsigned char order[RECMP_ORDER_MAX*RECMP_ORDER_BYTES];
        CHECK(applied && replayed);
        if (!r) { printf("reader: %s\n",why); CHECK(r); }
        CHECK(recr_header(r)->mode==3 && recr_header(r)->has_mp && recr_header(r)->has_mp_seats &&
              recr_header(r)->mp_seats.actor[3].origin==66 && recr_header(r)->mp_seats.actor[3].seat==3 &&
              recr_header(r)->mp_seats.actor[3].first==7 && recr_header(r)->mp_seats.actor[3].last==8);
        CHECK(recr_mp_player(r,66,&pstate,why,sizeof why)==0 &&
              recr_mp_unit(r,7,&ustate,why,sizeof why)==0 &&
              recr_mp_named(r,55,&nstate,why,sizeof why)==0 &&
              recr_mp_roots(r,55,&rstate,why,sizeof why)==0 &&
              recr_mp_orders(r,7,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==0);
        CHECK(recr_mp_apply(r,&ordinal,applied_copy,applied,why,sizeof why)==0 && !ordinal && !applied->count);
        CHECK(recr_tick(r,why,sizeof why)==1 && recr_world(r)->tick==100 && recr_world(r)->player[0][0]==0xA5);
        if (recr_header(r)->format>=REC_FORMAT_NET_CREATE) {
            RecNetCreate create={6,1,2,4,0x80000011u,{123456,-654321,123},0.25f,65535,0};
            CHECK(recr_world(r)->nev==1 && recr_world(r)->ev[0].kind==RK_EV_NET_CREATE &&
                  recr_world(r)->ev[0].len==sizeof create &&
                  !memcmp(recr_world(r)->ev[0].b,&create,sizeof create));
        } else CHECK(recr_header(r)->format==REC_FORMAT_OLDEST && !recr_world(r)->nev);
        for (unsigned i=0;i<4;i++) {
            unsigned origin=i==0 ? 11 : i==1 ? 22 : i==2 ? 55 : 66;
            if (expected) {
                CHECK(recr_mp_player(r,origin,&pstate,why,sizeof why)==1 && pstate.image[0]==origin &&
                      pstate.controller==1+(i&1) && pstate.source_tick==(i<2 ? 100 : 8) &&
                      pstate.sequence==2 && pstate.applied_tick==100 &&
                      ((unsigned char*)pstate.image)[0x146]==i);
                for (unsigned v=0;v<5;v++) for (unsigned row=0;row<11;row++) {
                    const unsigned sender[]={7,4,0,2,1,3,5,6,8,9,10};
                    CHECK(((unsigned char*)pstate.image)[0x108+11*v+row]==20*v+(i<2 ? row : sender[row])+1);
                }
            }
        }
        if (expected) {
            read_units(r,why,sizeof why);
            read_perspective(r,why,sizeof why);
            CHECK(recr_mp_orders(r,7,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 &&
                  ostate.count==0 && ostate.birth==17 && ostate.sequence==4 &&
                  ostate.source_tick==8 && ostate.applied_tick==100 && ostate.stream.Data1==2);
            CHECK(recr_mp_orders(r,8,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==0);
            CHECK(recr_mp_player(r,11,&pstate,why,sizeof why)==1 &&
                  recr_mp_orders(r,1,1,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 &&
                  ostate.origin==11 && ostate.birth==21 && ostate.count==1 && ostate.sequence==3 &&
                  ostate.source_tick==100 && ostate.applied_tick==100 &&
                  !memcmp(&ostate.stream,&pstate.stream,sizeof ostate.stream));
            for (unsigned i=0;i<RECMP_ORDER_BYTES;i++) CHECK(order[i]==(unsigned char)(i*7+3));
            read_applications(r,applied,&ordinal,why,sizeof why);
            CHECK(applied->count==19 && applied->roots==3);
            replay_ordinal=10;
            CHECK(recr_mp_apply(r,&replay_ordinal,applied_copy,replayed,why,sizeof why)==1 && replay_ordinal==19);
            applied_equal(applied,10,replayed);
            memset(replayed,0,sizeof *replayed);
            CHECK(recr_tick(r,why,sizeof why)==1 && recr_world(r)->tick==101 && recr_world(r)->player[0][0]==0xA5);
            read_units(r,why,sizeof why);
            CHECK(recr_mp_orders(r,7,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 &&
                  ostate.count==0 && ostate.sequence==4 && ostate.applied_tick==100);
            CHECK(recr_mp_player(r,66,&pstate,why,sizeof why)==1 && pstate.image[0]==66 && pstate.sequence==2 &&
                  ((unsigned char*)pstate.image)[0x146]==3);
            {
                RecMpExploreState e; unsigned char bit=0xEE;
                CHECK(recr_mp_explored(r,11,&e,&bit,1,why,sizeof why)==1 && bit==1 &&
                      e.cells==1 && e.bytes==1 && e.lostype==0xFFFF && e.source_tick==101 && e.applied_tick==101);
                CHECK(recr_mp_explored(r,22,&e,&bit,1,why,sizeof why)==1 && bit==0 && e.lostype==5);
                CHECK(recr_mp_explored(r,55,&e,&bit,1,why,sizeof why)==0);
            }
            read_perspective(r,why,sizeof why);
            read_applications(r,applied,&ordinal,why,sizeof why);
            CHECK(applied->count==34 && applied->roots==6 && applied->packet[22].h.origin==11 &&
                  applied->packet[23].h.origin==22 && applied->packet[24].h.origin==11 &&
                  applied->packet[28].h.kind==RECMP_POV && applied->packet[29].h.kind==RECMP_CLICK &&
                  applied->packet[30].h.kind==RECMP_SELECT && applied->packet[31].h.origin==22 &&
                  applied->packet[32].h.kind==RECMP_SEAL);
            replay_ordinal=0;
            CHECK(recr_mp_apply(r,&replay_ordinal,applied_copy,replayed,why,sizeof why)==1 && replay_ordinal==34);
            applied_equal(applied,0,replayed);
            memset(replayed,0,sizeof *replayed);
        }
        CHECK(!recr_tick(r,why,sizeof why));
        CHECK(recr_seek(r,100,0,NULL,NULL,why,sizeof why)==1 && recr_world(r)->player[0][0]==0xA5);
        if (expected) {
            uint64_t beyond=34;
            CHECK(recr_mp_apply(r,&beyond,applied_copy,replayed,why,sizeof why)==-1 && beyond==34 && !replayed->count);
            replay_ordinal=0;
            CHECK(recr_mp_apply(r,&replay_ordinal,applied_copy,replayed,why,sizeof why)==1 && replay_ordinal==19);
            CHECK(replayed->count==19 && replayed->roots==3);
            read_units(r,why,sizeof why);
            read_perspective(r,why,sizeof why);
            CHECK(recr_mp_player(r,66,&pstate,why,sizeof why)==1 && pstate.image[0]==66 && pstate.sequence==2);
            CHECK(recr_seek(r,101,0,NULL,NULL,why,sizeof why)==1 &&
                  recr_mp_orders(r,7,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 && ostate.sequence==4);
            read_units(r,why,sizeof why);
            read_perspective(r,why,sizeof why);
            CHECK(recr_mp_apply(r,&replay_ordinal,applied_copy,replayed,why,sizeof why)==1 && replay_ordinal==34);
            applied_equal(applied,0,replayed);
            CHECK(recr_mp_orders(r,1,1,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 && ostate.count==1);
            for (unsigned i=0;i<RECMP_ORDER_BYTES;i++) CHECK(order[i]==(unsigned char)(i*7+3));
            for (unsigned i=0;i<applied->count;i++) {
                RecMpPacket* p=&applied->packet[i];
                printf("APPLICATION %llu %u %u %u %u %u %u %u %08X\n",
                       (unsigned long long)applied->where[i].ordinal,applied->where[i].tick,
                       applied->where[i].phase,p->h.origin,p->h.tick,p->h.sequence,p->h.kind,
                       p->h.bytes,(unsigned)Crc32_ComputeBuf(0,p,p->h.bytes));
            }
            puts("PASS: all application batches remain ordered through interleaved queries, duplicate polls, resumed ordinals and seek; proof tails never visit");
        }
        CHECK(recr_summary(path,&summary,why,sizeof why) && summary.end_reason==(expected ? REC_END_TEARDOWN : 0));
        CHECK(recr_info(path,info,why,sizeof why) && !!info->ended==!!expected);
        recr_close(r);
        free(applied); free(replayed);
        puts(expected ? "PASS: actual .imprec writer/reader, world seek and persisted cut proof" :
                        "PASS: failed contribution file has no complete end, world prefix remains readable");
    }
    free(info);
    return 0;
}

typedef struct {
    const RecHeader* header;
    uint64_t records, bytes, roots, texts;
    unsigned crc, print_text;
} NativeRead;

static void native_application(void* ctx, const RecMpRecord* where, const RecMpHeader* packet,
                                 const void* body, unsigned bytes)
{
    NativeRead* n=ctx;
    unsigned i;
    CHECK(where->ordinal==++n->records && where->phase!=RECMP_RECORD_PROOF &&
          bytes<=RECMP_BODY_MAX && packet->bytes==sizeof *packet+bytes);
    for (i=0;i<n->header->mp.count;i++) if (n->header->mp.actor[i].origin==packet->origin) break;
    CHECK(i<n->header->mp.count &&
          where->phase==(n->header->mp.actor[i].local ? RECMP_RECORD_PRODUCED : RECMP_RECORD_CONSUMED));
    n->crc=(unsigned)Crc32_ComputeBuf(n->crc,where,sizeof *where);
    n->crc=(unsigned)Crc32_ComputeBuf(n->crc,packet,sizeof *packet);
    n->crc=(unsigned)Crc32_ComputeBuf(n->crc,body,bytes);
    n->bytes+=sizeof *where+packet->bytes;
    if (packet->kind==RECMP_ROOTS) n->roots++;
    if (packet->kind==RECMP_TEXT) {
        unsigned count;
        memcpy(&count,body,4);
        n->texts+=count;
        if (!n->print_text) return;
        for (unsigned j=0;j<count;j++) {
            RecMpTextEntry e;
            memcpy(&e,(const unsigned char*)body+4+j*sizeof e,sizeof e);
            printf("NATIVE_TEXT %llu %u %u %u %u %u %u %u %llu %u %u %u ",
                   (unsigned long long)where->ordinal,where->tick,packet->origin,packet->sequence,
                   packet->tick,e.phase,e.origin,e.recipient,(unsigned long long)e.event,
                   e.source_tick,e.display,e.mode);
            for (unsigned k=0;k<RECMP_ACTORS;k++) printf("%u%s",e.target[k],k+1==RECMP_ACTORS ? " " : ",");
            for (unsigned k=0;k<sizeof e.stock;k++) printf("%02X",e.stock[k]);
            puts("");
        }
    }
}

static void native_explored(RecReader* r, const char* stage)
{
    char why[256];
    const RecHeader* h=recr_header(r);
    for (unsigned i=0;i<h->mp_seats.count;i++) {
        RecMpExploreState state;
        unsigned char* image;
        int known=recr_mp_explored(r,h->mp_seats.actor[i].origin,&state,NULL,0,why,sizeof why);
        CHECK(known>=0);
        if (!known) continue;
        image=malloc(state.bytes); CHECK(image);
        CHECK(recr_mp_explored(r,state.origin,&state,image,state.bytes,why,sizeof why)==1);
        printf("NATIVE_EXPLORED %s %u %u %u %u %u %u %u %u %08X ",stage,
               recr_world(r)->tick,state.origin,state.source_tick,state.sequence,state.applied_tick,
               state.cells,state.bytes,state.lostype,(unsigned)Crc32_ComputeBuf(0,image,state.bytes));
        for (unsigned j=0;j<sizeof state.stream;j++) printf("%02X",((unsigned char*)&state.stream)[j]);
        puts(""); free(image);
    }
}

static void native_text_query(RecReader* r, const char* stage)
{
    char why[256];
    const RecHeader* h=recr_header(r);
    for (unsigned i=0;i<h->mp_seats.count;i++) {
        RecMpTextState state;
        int known=recr_mp_text(r,h->mp_seats.actor[i].origin,&state,why,sizeof why);
        CHECK(known>=0);
        if (!known) continue;
        printf("NATIVE_TEXT_QUERY %s %u %u %u %u %u %u %08X ",stage,
               recr_world(r)->tick,state.origin,state.source_tick,state.sequence,state.applied_tick,
               state.count,(unsigned)Crc32_ComputeBuf(0,state.body,state.bytes));
        for (unsigned j=0;j<sizeof state.stream;j++) printf("%02X",((unsigned char*)&state.stream)[j]);
        puts("");
    }
}

static int read_native(const char* path, unsigned text_only)
{
    char why[256];
    RecReader* r=recr_open(path,why,sizeof why);
    RecSummary summary;
    RecInfo* info=calloc(1,sizeof *info);
    NativeRead n={0}, replayed={0};
    uint64_t ordinal=0;
    unsigned first=0, last=0, creates=0, create_crc=0, seek_creates=0, seek_crc=0;
    int k;
    if (!r) printf("native reader: %s\n",why);
    CHECK(r && info && recr_header(r)->mode==3 && recr_header(r)->has_mp &&
          recr_summary(path,&summary,why,sizeof why) && summary.end_reason==REC_END_TEARDOWN &&
          recr_info(path,info,why,sizeof why) && info->ended);
    n.header=recr_header(r);
    n.print_text=1;
    while ((k=recr_tick(r,why,sizeof why))>0) {
        CHECK(k==1);
        if (!first) first=recr_world(r)->tick;
        last=recr_world(r)->tick;
        for (unsigned i=0;i<recr_world(r)->nev;i++) {
            const RecEvent* e=&recr_world(r)->ev[i];
            if (e->kind!=RK_EV_NET_CREATE) continue;
            creates++;
            create_crc=(unsigned)Crc32_ComputeBuf(create_crc,&last,sizeof last);
            create_crc=(unsigned)Crc32_ComputeBuf(create_crc,e->b,e->len);
        }
        CHECK(recr_mp_apply(r,&ordinal,native_application,&n,why,sizeof why)==1 && ordinal==n.records);
        CHECK(recr_mp_apply(r,&ordinal,native_application,&n,why,sizeof why)==1 && ordinal==n.records);
    }
    CHECK(!k && first && last>=first && n.records && (text_only ? n.texts : n.roots));
    native_explored(r,"final");
    native_text_query(r,"final");
    for (unsigned i=0;i<n.header->mp_seats.count;i++) {
        const RecMpSeat* seat=&n.header->mp_seats.actor[i];
        RecMpPlayerState player;
        RecMpUnitState unit;
        unsigned char* image;
        CHECK(recr_mp_player(r,seat->origin,&player,why,sizeof why)==1);
        image=(unsigned char*)player.image;
        CHECK(image[0x146]==seat->seat &&
              recr_mp_unit(r,seat->first,&unit,why,sizeof why)==1 && unit.origin==seat->origin &&
              recr_mp_unit(r,seat->last,&unit,why,sizeof why)==1 && unit.origin==seat->origin);
    }
    /* A world seek and state queries cannot acknowledge any application batch.
       Reconstructing from its prefix, then advancing, must deliver every byte. */
    CHECK(recr_seek(r,first+(last-first)/2,0,NULL,NULL,why,sizeof why)==1);
    native_explored(r,"seek-middle");
    native_text_query(r,"seek-middle");
    replayed.header=recr_header(r); ordinal=0;
    CHECK(recr_mp_apply(r,&ordinal,native_application,&replayed,why,sizeof why)==1);
    while ((k=recr_tick(r,why,sizeof why))>0)
        CHECK(k==1 && recr_mp_apply(r,&ordinal,native_application,&replayed,why,sizeof why)==1);
    CHECK(!k && n.records==replayed.records && n.bytes==replayed.bytes &&
          n.roots==replayed.roots && n.texts==replayed.texts && n.crc==replayed.crc);
    native_explored(r,"seek-final");
    native_text_query(r,"seek-final");
    printf("NATIVE_APPLICATIONS %llu %llu %llu %08X %u %u\n",
           (unsigned long long)n.records,(unsigned long long)n.bytes,(unsigned long long)n.roots,
           n.crc,first,last);
    CHECK(recr_seek(r,first,0,NULL,NULL,why,sizeof why)==1);
    do {
        const RecWorld* w=recr_world(r);
        for (unsigned i=0;i<w->nev;i++) {
            const RecEvent* e=&w->ev[i];
            if (e->kind!=RK_EV_NET_CREATE) continue;
            seek_creates++;
            seek_crc=(unsigned)Crc32_ComputeBuf(seek_crc,&w->tick,sizeof w->tick);
            seek_crc=(unsigned)Crc32_ComputeBuf(seek_crc,e->b,e->len);
        }
        k=recr_tick(r,why,sizeof why);
    } while (k>0);
    CHECK(!k && creates==seek_creates && create_crc==seek_crc);
    printf("NATIVE_CREATES %u %08X\n",creates,create_crc);
    recr_close(r); free(info);
    puts("PASS: native world/header/info and complete ordered authority through sequential reading and world seek");
    return 0;
}

/* A skirmish file's perspective through the real writer and reader: player 0's samples every
   third tick, a selection each 200 ticks and a click each 250, so the writer's snapshot after
   PV_FIRST + 900 carries two dated samples and a selection. Forward reading keeps each tick's
   state; seeks before and after the snapshot must rebuild exactly that state. */
#define PV_FIRST 100u
#define PV_LAST  1100u
typedef struct PvState {
    unsigned npov, tick[2], nsel, nclick;
    RecPov pov[2];
    RecSelEntry sel[4];
} PvState;

static void pv_state(const RecWorld* w, PvState* s)
{
    memset(s,0,sizeof *s);
    s->npov=w->npov[0]; s->nsel=w->nsel[0]; s->nclick=w->nclick;
    CHECK(s->npov<=2 && s->nsel<=4 && !w->npov[1] && !w->nsel[1]);
    for (unsigned i=0;i<s->npov;i++) { s->pov[i]=w->pov[0][i]; s->tick[i]=w->pov_tick[0][i]; }
    memcpy(s->sel,w->sel,s->nsel*sizeof *s->sel);
}

static int perspective_file(const char* path)
{
    RecRing* g=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *g);
    unsigned char header[1024], player[4+REC_PLAYER_IMAGE]={0}, seat[16+REC_INFO_BYTES]={0};
    unsigned globals[REC_GLOBALS_DWORDS]={0}, proj[2]={0}, mapped[2]={0};
    unsigned features[5]={2,2,1,0,0}, at=0, n=0, first=2, last=5, k, t, T;
    unsigned short grid[2]={2,2};
    static const unsigned SEEKS[]={PV_FIRST+450,PV_FIRST+901,PV_FIRST+950,PV_LAST-1,PV_LAST};
    PvState* fwd=calloc(PV_LAST-PV_FIRST+1,sizeof *fwd);
    PvState now;
    RecReader* r;
    HANDLE done;
    char why[256];
    CHECK(g && fwd);
    g->buf=VirtualAlloc(NULL,REC_RING_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g->grid=VirtualAlloc(NULL,4*REC_CELL_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    g->wake=CreateEvent(NULL,FALSE,FALSE,NULL); g->done=CreateEvent(NULL,TRUE,FALSE,NULL);
    snprintf(g->path,sizeof g->path,"%s",path);
    CHECK(g->buf && g->grid && g->wake && g->done && DuplicateHandle(GetCurrentProcess(),g->done,
        GetCurrentProcess(),&done,0,FALSE,DUPLICATE_SAME_ACCESS));
    n=tlv32(header,n,RT_FORMAT,REC_FORMAT); n=tlv32(header,n,RT_MODE,2);
    n=tlv32(header,n,RT_SLOTS,21); n=tlv32(header,n,RT_UNITS_EACH,4); n=tlv32(header,n,RT_START,PV_FIRST);
    n=tlv32(header,n,RT_PROJECTILES,1);
    n=append(header,n,RT_FEATURES,features,sizeof features);
    for (unsigned i=0;i<2;i++) {
        seat[0]=(unsigned char)i; seat[1]=(unsigned char)(i+1);
        n=append(header,n,RT_PLAYER,seat,sizeof seat);
    }
    at=append(g->buf,at,RK_HEADER,header,n);
    memcpy(player+4+0x67,&first,4); memcpy(player+4+0x6B,&last,4);
    for (t=PV_FIRST;t<=PV_LAST;t++) {
        at=append(g->buf,at,RK_TICK,&t,4);
        if (t==PV_FIRST) {
            at=append(g->buf,at,RK_GRID,grid,sizeof grid);
            at=append(g->buf,at,RK_GLOBALS,globals,sizeof globals);
            at=append(g->buf,at,RK_PROJ_HEAD,proj,sizeof proj);
            at=append(g->buf,at,RK_MAPPED,mapped,6);
            at=append(g->buf,at,RK_PLAYER,player,sizeof player);
        }
        if (t%3==1) {
            RecPov p={0,REC_POV_EVERY,REC_POV_WORLD,0,640,480,65536u+t,(int32_t)t,-(int32_t)t,
                      (int32_t)t+1,0,(int32_t)t+2};
            at=append(g->buf,at,RK_POV,&p,sizeof p);
        }
        if (t%200==0) {
            unsigned char b[REC_SEL_BYTES(4)];
            RecSel h={0,REC_SEL_BEGIN|REC_SEL_END,0,0,0};
            h.total=h.count=(uint16_t)(t/200%4+1);
            memcpy(b,&h,sizeof h);
            for (k=0;k<h.count;k++) {
                RecSelEntry e={(uint16_t)(k+1),t+k};
                memcpy(b+sizeof h+k*sizeof e,&e,sizeof e);
            }
            at=append(g->buf,at,RK_SELECT,b,REC_SEL_BYTES(h.count));
        }
        if (t%250==0) {
            RecClick c={0,REC_CLICK_RIGHT,1,3,2,0,4,(int32_t)t,0,-(int32_t)t};
            at=append(g->buf,at,RK_CLICK,&c,sizeof c);
        }
    }
    CHECK(at<REC_RING_BYTES/2);
    InterlockedExchange(&g->wpub,at);
    CHECK(tagpu_recwriter_start(g));
    world_end(g,at);
    CHECK(WaitForSingleObject(done,30000)==WAIT_OBJECT_0); CloseHandle(done);

    r=recr_open(path,why,sizeof why);
    if (!r) { printf("reader: %s\n",why); CHECK(r); }
    for (t=PV_FIRST;;t++) {
        int got=recr_tick(r,why,sizeof why);
        if (got!=1) { CHECK(got==0 && t==PV_LAST+1); break; }
        CHECK(recr_world(r)->tick==t);
        pv_state(recr_world(r),&fwd[t-PV_FIRST]);
        CHECK(fwd[t-PV_FIRST].nclick==(t%250==0));
        if (t%250==0) {
            RecClick c;
            memcpy(&c,recr_world(r)->click[0].b,sizeof c);
            CHECK(recr_world(r)->click[0].kind==RK_CLICK && c.button==REC_CLICK_RIGHT && c.x==(int32_t)t);
        }
    }
    CHECK(fwd[PV_LAST-PV_FIRST].npov==2 && fwd[PV_LAST-PV_FIRST].tick[1]==PV_LAST-(PV_LAST+2)%3);
    for (k=0;k<sizeof SEEKS/sizeof SEEKS[0];k++) {
        T=SEEKS[k];
        CHECK(recr_seek(r,T,0,NULL,NULL,why,sizeof why)==1 && recr_world(r)->tick==T);
        pv_state(recr_world(r),&now);
        if (memcmp(&now,&fwd[T-PV_FIRST],sizeof now)) {
            printf("PERSPECTIVE seek to %u: npov %u/%u nsel %u/%u\n",T,now.npov,fwd[T-PV_FIRST].npov,
                   now.nsel,fwd[T-PV_FIRST].nsel);
            CHECK(!"the seek rebuilt the forward perspective");
        }
    }
    recr_close(r);
    free(fwd);
    printf("PASS: perspective samples, clicks and selections through the writer, forward reading "
           "and %u seeks\n",(unsigned)(sizeof SEEKS/sizeof SEEKS[0]));
    return 0;
}

#ifndef RECMP_FILE_TEST_MAIN
#define RECMP_FILE_TEST_MAIN main
#endif
int RECMP_FILE_TEST_MAIN(int argc, char** argv)
{
    RecMpConfig config={0};
    RecMpRoster roster={ .version=1, .count=4, .slots=21, .each=2 }, bad;
    RecMpChannel* c;
    RecMpFile* mp;
    RecRing* ring;
    unsigned write;
    HANDLE done;
    RecMpSignedIdentity binding;
    RecMpIdentity remote={RECMP_VERSION,sizeof remote,{.Data1=1},{.Data1=3},202};
    GUID stream={.Data1=2};
    RecMpPacket got;
    RecMpStats stats;
    int local_stop;
    CHECK(argc==3);
    proof_only_roster();
    if (!strcmp(argv[1],"native")) return read_native(argv[2],0);
    if (!strcmp(argv[1],"native-text")) return read_native(argv[2],1);
    if (!strcmp(argv[1],"world-refuse")) {
        char why[256];
        RecReader* r=recr_open(argv[2],why,sizeof why);
        CHECK(r && recr_tick(r,why,sizeof why)==-1 && !recr_world(r)->nev);
        printf("PASS: malformed network create refused before event publication: %s\n",why);
        recr_close(r);
        return 0;
    }
    if (!strcmp(argv[1],"skirmish")) {
        char why[256];
        RecReader* r=recr_open(argv[2],why,sizeof why);
        RecSummary summary;
        RecInfo* info=calloc(1,sizeof *info);
        CHECK(r && info && recr_header(r)->mode==2 && !recr_header(r)->has_mp);
        CHECK(recr_summary(argv[2],&summary,why,sizeof why) && recr_info(argv[2],info,why,sizeof why));
        CHECK(summary.nseats==recr_header(r)->nseats && info->ended && summary.end_reason);
        uint64_t ordinal=0;
        Applied* a=calloc(1,sizeof *a); CHECK(a);
        CHECK(recr_tick(r,why,sizeof why)==1 &&
              recr_mp_apply(r,&ordinal,applied_copy,a,why,sizeof why)==0 && !ordinal && !a->count);
        free(a);
        recr_close(r); free(info);
        puts("PASS: actual skirmish recording's full header validation, reader, summary and file info");
        return 0;
    }
    if (!strcmp(argv[1],"perspective")) return perspective_file(argv[2]);
    if (!strcmp(argv[1],"read")) return read_file(argv[2],1);
    if (!strcmp(argv[1],"refuse")) return read_file(argv[2],-1);
    f.missing_cut=!strcmp(argv[1],"missing_cut");
    f.missing_receipt=!strcmp(argv[1],"missing_receipt");
    f.journal_full=!strcmp(argv[1],"journal_full");
    local_stop=!strcmp(argv[1],"local_stop");
    f.game_thread=GetCurrentThreadId();
    f.net.identity=remote; f.net.identity.player=101;
    f.net.stats.peers=1; f.net.stats.membership=1;
    f.opened=CreateEvent(NULL,TRUE,FALSE,NULL); f.release=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.initial=CreateEvent(NULL,TRUE,FALSE,NULL); f.tail_release=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.net.event=CreateEvent(NULL,FALSE,FALSE,NULL);
    CHECK(f.opened && f.release && f.initial && f.tail_release && f.net.event);
    config.signer=tagpu_recmp_key(); CHECK(config.signer);
    config.origin=11; config.count=4; config.net.match.Data1=1;
    config.net.host=1; config.net.address_bytes=3; memcpy(config.net.address,"ip!",3);
    for (unsigned i=0;i<4;i++) {
        config.actor[i].origin=i==0 ? 11 : i==1 ? 22 : i==2 ? 55 : 66;
        config.actor[i].local=i<2; tagpu_recmp_public(config.signer,&config.actor[i].public);
        roster.actor[i]=(RecMpSeat){config.actor[i].origin,i,1+2*i,2+2*i};
    }
    f.config=config;
    for (unsigned i=0;i<2;i++) {
        CHECK(tagpu_recmp_sign(config.signer,i ? 66 : 55,&stream,&remote,&binding));
        packet(i ? 66 : 55,RECMP_BIND,0,0,&binding,sizeof binding);
        {
            RecMpRoster r=source_roster(1);
            RecMpPacket* p=packet(i ? 66 : 55,RECMP_SEATS,1,8,&r,sizeof r);
            CHECK(tagpu_recmp_stream_put(&f.source[i],&p->h,p->body,sizeof r));
        }
        source_player(i ? 66 : 55,2,8,1);
        if (i) source_units(3,8,1);
        source_orders(i ? 66 : 55,i ? 4 : 3,8,0,0,0);
        if (!i) { source_named(4,0); source_roots(5,0); }
    }
    source_orders(66,5,9,46,0,45);
    c=tagpu_recmp_begin(&config); CHECK(c);
    bad=roster; bad.actor[0].origin=99;
    CHECK(!tagpu_recmp_file_begin(&config,c,&bad));
    mp=tagpu_recmp_file_begin(&config,c,&roster); CHECK(mp);
    memset(&roster,0,sizeof roster);
    CHECK(tagpu_recmp_file_roster(mp)->count==4 && tagpu_recmp_file_roster(mp)->actor[3].last==8);
    if (f.journal_full) {
        /* Before the writer starts, a deterministic full journal. */
        RecMpPacket p=f.packet[1];
        unsigned char body[RECMP_BODY_MAX]={0};
        p.h.origin=11; p.h.bytes=RECMP_PACKET_MAX;
        while (tagpu_recmp_record_put(tagpu_recmp_file_journal(mp),&p.h,body,sizeof body,100,1)) {}
    }
    done=world_start(mp,argv[2],&ring,&write);
    CHECK(WaitForSingleObject(f.opened,15000)==WAIT_OBJECT_0);
    memset(&config,0,sizeof config); SetEvent(f.release);
    CHECK(WaitForSingleObject(f.initial,15000)==WAIT_OBJECT_0);
    for (unsigned i=0;i<9;i++) CHECK(tagpu_recmp_take_at(c,&got,100)==1);
    CHECK(tagpu_recmp_advance(c,100));
    for (unsigned i=0;i<2;i++) {
        unsigned char body[sizeof(RecMpPlayerDelta)+REC_PLAYER_IMAGE];
        unsigned origin=i ? 22 : 11, n=player_body(origin,1,body);
        RecMpRoster r=source_roster(0);
        CHECK(tagpu_recmp_send_from(c,origin,RECMP_SEATS,100,&r,sizeof r,NULL));
        CHECK(tagpu_recmp_send_from(c,origin,RECMP_PLAYER,100,body,n,NULL));
        if (i) { n=unit_body(origin,1,body); CHECK(tagpu_recmp_send_from(c,origin,RECMP_UNITS,100,body,n,NULL)); }
    }
    {
        RecMpOrders chunk={1,1,0,21,1,0,1,0};
        unsigned char body[sizeof chunk+RECMP_ORDER_BYTES];
        memcpy(body,&chunk,sizeof chunk);
        for (unsigned i=0;i<RECMP_ORDER_BYTES;i++) body[sizeof chunk+i]=(unsigned char)(i*7+3);
        CHECK(tagpu_recmp_send_from(c,11,RECMP_ORDERS,100,body,sizeof body,NULL));
    }
    for (unsigned i=0;i<2;i++) {
        unsigned char body[RECMP_BODY_MAX];
        unsigned origin=i ? 22 : 11, n=named_body(origin,0,body);
        CHECK(tagpu_recmp_send_from(c,origin,RECMP_NAMED,100,body,n,NULL));
        n=roots_body(origin,0,body);
        CHECK(tagpu_recmp_send_from(c,origin,RECMP_ROOTS,100,body,n,NULL));
    }
    CHECK(tagpu_recmp_advance(c,101) && tagpu_recmp_take_at(c,&got,101)==1);
    for (unsigned i=0;i<3;i++) {
        unsigned char body[RECMP_BODY_MAX];
        unsigned origin=i==1 ? 22 : 11, n=roots_body(origin,i==2 ? 2 : 1,body);
        CHECK(tagpu_recmp_send_from(c,origin,RECMP_ROOTS,101,body,n,NULL));
    }
    for (unsigned i=0;i<3;i++) {
        struct { RecMpExploreChunk chunk; unsigned char bit; } packet={{0,1,1,i==2 ? 0xFFFF : 5,i==2 ? 3 : 7,{0}},i==2 ? 1 : 0};
        CHECK(tagpu_recmp_send_from(c,i==1 ? 22 : 11,RECMP_EXPLORED,101,&packet,sizeof packet,NULL));
    }
    {
        /* the perspective kinds: 11's sample, click and two-unit selection, 22's one-unit one */
        RecPov p={RECMP_POV_PLAYER,RECMP_POV_EVERY,REC_POV_WORLD|REC_POV_MINIMAP,0,640,480,98304,
                  321,-654,30,4,-40};
        RecClick k={RECMP_POV_PLAYER,REC_CLICK_LEFT,2,5,3,1,8,31,5,-41};
        RecSel s={RECMP_POV_PLAYER,REC_SEL_BEGIN|REC_SEL_END,2,0,2};
        RecSelEntry e[2]={{1,21},{2,22}};
        unsigned char body[REC_SEL_BYTES(2)];
        memcpy(body,&s,sizeof s); memcpy(body+sizeof s,e,sizeof e);
        CHECK(tagpu_recmp_send_from(c,11,RECMP_POV,101,&p,sizeof p,NULL));
        CHECK(tagpu_recmp_send_from(c,11,RECMP_CLICK,101,&k,sizeof k,NULL));
        CHECK(tagpu_recmp_send_from(c,11,RECMP_SELECT,101,body,sizeof body,NULL));
        s.total=s.count=1; e[0]=(RecSelEntry){3,33};
        memcpy(body,&s,sizeof s); memcpy(body+sizeof s,e,sizeof *e);
        CHECK(tagpu_recmp_send_from(c,22,RECMP_SELECT,101,body,(unsigned)REC_SEL_BYTES(1),NULL));
    }
    tagpu_recmp_retain(c);
    if (!local_stop) tagpu_recmp_end(c);
    tagpu_recmp_file_end(mp,!local_stop); /* final game access, before ring hand-over */
    world_end(ring,write);
    if (!local_stop) SetEvent(f.tail_release);
    CHECK(WaitForSingleObject(done,30000)==WAIT_OBJECT_0); CloseHandle(done);
    if (local_stop) {
        CHECK(tagpu_recmp_send(c,RECMP_RESOURCES,101,"still playing",13));
        tagpu_recmp_end(c); SetEvent(f.tail_release);
    }
    tagpu_recmp_stats(c,&stats);
    if (f.missing_cut || f.missing_receipt) CHECK(stats.failed && stats.finished);
    else {
        DWORD deadline=GetTickCount()+30000;
        while (!stats.finished) {
            CHECK((LONG)(deadline-GetTickCount())>0); Sleep(1); tagpu_recmp_stats(c,&stats);
        }
        CHECK(!stats.failed && stats.cuts==2 && stats.receipts==4);
    }
    tagpu_recmp_release(c);
    CloseHandle(f.opened); CloseHandle(f.release); CloseHandle(f.initial);
    CloseHandle(f.tail_release); CloseHandle(f.net.event);
    return read_file(argv[2],!(f.missing_cut || f.missing_receipt || f.journal_full || local_stop));
}
