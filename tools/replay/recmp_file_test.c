/* Actual writer/reader fixture using the controlled native channel boundaries. */
static void file_proof_tail(void);
#define RECMP_FILE_HISTORY
#define main cut_test_main
#include "recmp_cut_test.c"
#undef main
#include "tagpu_recorder.h"
#include "tagpu_recreader.h"

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

static void file_proof_tail(void)
{
    for (unsigned i=0;i<2;i++) {
        RecMpSeal seal;
        RecMpPacket* p;
        if (f.missing_cut && i==1) continue;
        if (i==1) {
            source_orders(66,4,9,46,45,1);
            source_player(66,5,10,0);
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
    unsigned features[5]={2,2,1,0,0}, tick=100, at=0, n=0;
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
        unsigned char order[RECMP_ORDER_MAX*RECMP_ORDER_BYTES];
        if (!r) { printf("reader: %s\n",why); CHECK(r); }
        CHECK(recr_header(r)->mode==3 && recr_header(r)->has_mp && recr_header(r)->has_mp_seats &&
              recr_header(r)->mp_seats.actor[3].origin==66 && recr_header(r)->mp_seats.actor[3].seat==3 &&
              recr_header(r)->mp_seats.actor[3].first==7 && recr_header(r)->mp_seats.actor[3].last==8);
        CHECK(recr_mp_player(r,66,&pstate,why,sizeof why)==0 &&
              recr_mp_orders(r,7,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==0);
        CHECK(recr_tick(r,why,sizeof why)==1 && recr_world(r)->tick==100 && recr_world(r)->player[0][0]==0xA5);
        for (unsigned i=0;i<4;i++) {
            unsigned origin=i==0 ? 11 : i==1 ? 22 : i==2 ? 55 : 66;
            if (expected) {
                CHECK(recr_mp_player(r,origin,&pstate,why,sizeof why)==1 && pstate.image[0]==origin &&
                      pstate.controller==1+(i&1) && pstate.source_tick==(i<2 ? 100 : 8) &&
                      pstate.sequence==1 && pstate.applied_tick==100);
            }
        }
        if (expected) {
            CHECK(recr_mp_orders(r,7,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 &&
                  ostate.count==0 && ostate.birth==17 && ostate.sequence==2 &&
                  ostate.source_tick==8 && ostate.applied_tick==100 && ostate.stream.Data1==2);
            CHECK(recr_mp_orders(r,8,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==0);
            CHECK(recr_mp_player(r,11,&pstate,why,sizeof why)==1 &&
                  recr_mp_orders(r,1,1,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 &&
                  ostate.origin==11 && ostate.birth==21 && ostate.count==1 && ostate.sequence==2 &&
                  ostate.source_tick==100 && ostate.applied_tick==100 &&
                  !memcmp(&ostate.stream,&pstate.stream,sizeof ostate.stream));
            for (unsigned i=0;i<RECMP_ORDER_BYTES;i++) CHECK(order[i]==(unsigned char)(i*7+3));
            CHECK(recr_tick(r,why,sizeof why)==1 && recr_world(r)->tick==101 && recr_world(r)->player[0][0]==0xA5);
            CHECK(recr_mp_orders(r,7,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 &&
                  ostate.count==0 && ostate.sequence==2 && ostate.applied_tick==100);
            CHECK(recr_mp_player(r,66,&pstate,why,sizeof why)==1 && pstate.image[0]==66 && pstate.sequence==1);
        }
        CHECK(!recr_tick(r,why,sizeof why));
        CHECK(recr_seek(r,100,0,NULL,NULL,why,sizeof why)==1 && recr_world(r)->player[0][0]==0xA5);
        if (expected) {
            CHECK(recr_mp_player(r,66,&pstate,why,sizeof why)==1 && pstate.image[0]==66 && pstate.sequence==1);
            CHECK(recr_seek(r,101,0,NULL,NULL,why,sizeof why)==1 &&
                  recr_mp_orders(r,7,0,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 && ostate.sequence==2);
            CHECK(recr_mp_orders(r,1,1,&ostate,order,RECMP_ORDER_MAX,why,sizeof why)==1 && ostate.count==1);
            for (unsigned i=0;i<RECMP_ORDER_BYTES;i++) CHECK(order[i]==(unsigned char)(i*7+3));
        }
        CHECK(recr_summary(path,&summary,why,sizeof why) && summary.end_reason==(expected ? REC_END_TEARDOWN : 0));
        CHECK(recr_info(path,info,why,sizeof why) && !!info->ended==!!expected);
        recr_close(r);
        puts(expected ? "PASS: actual .imprec writer/reader, world seek and persisted cut proof" :
                        "PASS: failed contribution file has no complete end, world prefix remains readable");
    }
    free(info);
    return 0;
}

int main(int argc, char** argv)
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
    if (!strcmp(argv[1],"skirmish")) {
        char why[256];
        RecReader* r=recr_open(argv[2],why,sizeof why);
        RecSummary summary;
        RecInfo* info=calloc(1,sizeof *info);
        CHECK(r && info && recr_header(r)->mode==2 && !recr_header(r)->has_mp);
        CHECK(recr_summary(argv[2],&summary,why,sizeof why) && recr_info(argv[2],info,why,sizeof why));
        CHECK(summary.nseats==recr_header(r)->nseats && info->ended && summary.end_reason);
        recr_close(r); free(info);
        puts("PASS: actual skirmish recording's full header validation, reader, summary and file info");
        return 0;
    }
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
        source_player(i ? 66 : 55,1,8,1);
        source_orders(i ? 66 : 55,2,8,0,0,0);
    }
    source_orders(66,3,9,46,0,45);
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
    for (unsigned i=0;i<4;i++) CHECK(tagpu_recmp_take_at(c,&got,100)==1);
    CHECK(tagpu_recmp_advance(c,100));
    for (unsigned i=0;i<2;i++) {
        unsigned char body[sizeof(RecMpPlayerDelta)+REC_PLAYER_IMAGE];
        unsigned origin=i ? 22 : 11, n=player_body(origin,1,body);
        CHECK(tagpu_recmp_send_from(c,origin,RECMP_PLAYER,100,body,n,NULL));
    }
    {
        RecMpOrders chunk={1,1,0,21,1,0,1,0};
        unsigned char body[sizeof chunk+RECMP_ORDER_BYTES];
        memcpy(body,&chunk,sizeof chunk);
        for (unsigned i=0;i<RECMP_ORDER_BYTES;i++) body[sizeof chunk+i]=(unsigned char)(i*7+3);
        CHECK(tagpu_recmp_send_from(c,11,RECMP_ORDERS,100,body,sizeof body,NULL));
    }
    CHECK(tagpu_recmp_advance(c,101) && tagpu_recmp_take_at(c,&got,101)==1);
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
