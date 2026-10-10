/* Production cut/receipt/lifetime controls at the native backend boundary.
   Event publications choose the interleaving; no elapsed-time success proof. */
#define INITGUID
#define COBJMACROS
#include "tagpu_recmp_channel.h"
#include "tagpu_recmp_stream.h"
#include "tagpu_recmp_record.h"
#include "tagpu_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define tagpu_recmp_net_open native_open
#define tagpu_recmp_net_send native_send
#define tagpu_recmp_net_receive native_receive
#define tagpu_recmp_net_close native_close
#define tagpu_recmp_net_abort native_abort
#include "../../tagpu/ddraw/src/tagpu_recmp_net.c"
#undef tagpu_recmp_net_open
#undef tagpu_recmp_net_send
#undef tagpu_recmp_net_receive
#undef tagpu_recmp_net_close
#undef tagpu_recmp_net_abort

#define CHECK(x) do { if (!(x)) { printf("FAIL line %u: %s\n",__LINE__,#x); fflush(stdout); exit(1); } } while (0)
/* More than the channel's 256 receive slots, for the flood modes */
#define FLOOD 300u
#define PACKETS (FLOOD + 32u)

static struct {
    RecMpNet net;
    RecMpConfig config;
    RecMpStream source[2];
    RecMpPacket packet[PACKETS];
    unsigned count, read, tail, bindings, final_sent, receipts_sent, aborted;
    int missing_receipt, missing_cut, bad_receipt, journal_full, relinquish, rx_full, no_consumer;
    DWORD game_thread;
    HANDLE opened, release, initial, tail_release;
} f;

void tagpu_logf(const char* fmt, ...)
{
    va_list args;
    va_start(args,fmt); vprintf(fmt,args); va_end(args); putchar('\n');
}

static RecMpPacket* packet(unsigned origin, unsigned kind, unsigned sequence,
                            unsigned tick, const void* body, unsigned bytes)
{
    RecMpPacket* p;
    CHECK(f.count < PACKETS && bytes <= RECMP_BODY_MAX);
    p = &f.packet[f.count++]; memset(p,0,sizeof *p);
    memcpy(p->h.magic,"IMPUREMP",8);
    p->h.version=RECMP_VERSION; p->h.header=sizeof p->h; p->h.bytes=sizeof p->h+bytes;
    p->h.match=f.config.net.match; p->h.stream.Data1=2;
    p->h.origin=origin; p->h.kind=kind; p->h.sequence=sequence; p->h.tick=tick;
    if (bytes) memcpy(p->body,body,bytes);
    return p;
}

HRESULT tagpu_recmp_net_open(const RecMpNetConfig* config, RecMpNet** out)
{
    CHECK(GetCurrentThreadId()!=f.game_thread);
    SetEvent(f.opened);
    CHECK(WaitForSingleObject(f.release,15000)==WAIT_OBJECT_0);
    CHECK(!memcmp(config,&f.config.net,sizeof *config));
    *out=&f.net; return DP_OK;
}

HRESULT tagpu_recmp_net_send(RecMpNet* net, const void* bytes, unsigned size)
{
    const RecMpPacket* p=bytes;
    CHECK(net==&f.net && GetCurrentThreadId()!=f.game_thread && size==p->h.bytes);
    CHECK(p->h.origin==11 || p->h.origin==22);
    if (p->h.kind==RECMP_BIND) f.bindings |= p->h.origin==11 ? 1 : 2;
    else {
        CHECK(f.bindings==3);
        if (p->h.kind==RECMP_SEAL) {
            RecMpSeal seal; RecMpReceipt receipt;
            unsigned i;
            CHECK(size==sizeof p->h+sizeof seal);
            memcpy(&seal,p->body,sizeof seal);
            if (!seal.final) goto sent;
            receipt.origin=p->h.origin; receipt.sequence=p->h.sequence;
            receipt.stream=p->h.stream; receipt.seal=seal;
            for (i=0;i<2;i++) {
                if (f.missing_receipt && i==1 && p->h.origin==22) continue;
                if (f.bad_receipt && i==0 && p->h.origin==11) receipt.seal.crc ^= 1;
                packet(i ? 66 : 55,RECMP_RECEIPT,0,0,&receipt,sizeof receipt);
            }
            f.final_sent++;
        } else if (p->h.kind==RECMP_RECEIPT) f.receipts_sent++;
        else CHECK(p->h.kind==RECMP_RESOURCES || p->h.kind==RECMP_PLAYER ||
                   p->h.kind==RECMP_ORDERS || p->h.kind==RECMP_SEATS || p->h.kind==RECMP_UNITS ||
                   p->h.kind==RECMP_NAMED || p->h.kind==RECMP_ROOTS || p->h.kind==RECMP_EXPLORED ||
                   p->h.kind==RECMP_POV || p->h.kind==RECMP_CLICK || p->h.kind==RECMP_SELECT);
    }
sent:
    net->stats.completed++; net->stats.completed_bytes+=size;
    return DP_OK;
}

/* Source data in order, each packet also in its source's ledger for the later seal */
static void flood(unsigned at, unsigned origin, unsigned tick)
{
    unsigned n;
    for (n=0;n<FLOOD;n++) {
        RecMpPacket* p=packet(origin,RECMP_RESOURCES,f.source[at].sequence+1,tick,"flood",5);
        CHECK(tagpu_recmp_stream_put(&f.source[at],&p->h,p->body,5));
    }
}

static void proof_tail(void)
{
#ifdef RECMP_FILE_HISTORY
    file_proof_tail();
#else
    RecMpSeal seal; RecMpPacket* p;
    unsigned i;
    if (f.no_consumer) flood(1,66,9);
    for (i=0;i<2;i++) {
        if (f.missing_cut && i==1) continue;
        if (i==1) {
            p=packet(66,RECMP_RESOURCES,f.source[i].sequence+1,9,"later",5);
            CHECK(tagpu_recmp_stream_put(&f.source[i],&p->h,p->body,5));
        }
        CHECK(tagpu_recmp_stream_seal(&f.source[i],i ? 9 : 8,0,&seal));
        p=packet(i ? 66 : 55,RECMP_SEAL,f.source[i].sequence+1,i ? 9 : 8,&seal,sizeof seal);
        CHECK(tagpu_recmp_stream_put(&f.source[i],&p->h,p->body,sizeof seal));
    }
#endif
}

HRESULT tagpu_recmp_net_receive(RecMpNet* net, DWORD* from, void* bytes, unsigned* size)
{
    CHECK(net==&f.net && GetCurrentThreadId()!=f.game_thread);
    if (!f.tail && WaitForSingleObject(f.tail_release,0)==WAIT_OBJECT_0) {
        f.tail=1; proof_tail();
    }
    if (f.read==f.count) { SetEvent(f.initial); return DPERR_NOMESSAGES; }
    CHECK(*size>=f.packet[f.read].h.bytes);
    *size=f.packet[f.read].h.bytes; memcpy(bytes,&f.packet[f.read++],*size); *from=202;
    return DP_OK;
}

HRESULT tagpu_recmp_net_close(RecMpNet* net)
{
    CHECK(net==&f.net && !net->stats.pending && f.final_sent==2);
    return DP_OK;
}

void tagpu_recmp_net_abort(RecMpNet* net)
{
    CHECK(net==&f.net && GetCurrentThreadId()!=f.game_thread); f.aborted=1;
}

/* Waits for the worker's last store. The deadline only catches a hang: whether the channel
   failed, and what it holds, is read from the stats after the finish. */
static void channel_finished(RecMpChannel* c, RecMpStats* stats)
{
    DWORD deadline=GetTickCount()+30000;
    for (tagpu_recmp_stats(c,stats); !stats->finished; tagpu_recmp_stats(c,stats)) {
        CHECK((LONG)(deadline-GetTickCount())>0); Sleep(1);
    }
}

int main(int argc, char** argv)
{
    RecMpConfig config;
    RecMpChannel* c;
    RecMpJournal* journal;
    RecMpSignedIdentity binding;
    RecMpIdentity remote={RECMP_VERSION,sizeof remote,{.Data1=1},{.Data1=3},202};
    GUID stream={.Data1=2};
    RecMpStats stats;
    RecMpPacket got;
    RecMpCutPoint cut, frozen;
    unsigned i, drained=0;
    f.missing_receipt=argc>1 && !strcmp(argv[1],"missing_receipt");
    f.missing_cut=argc>1 && !strcmp(argv[1],"missing_cut");
    f.bad_receipt=argc>1 && !strcmp(argv[1],"bad_receipt");
    f.journal_full=argc>1 && !strcmp(argv[1],"journal_full");
    f.relinquish=argc>1 && !strcmp(argv[1],"relinquish");
    f.rx_full=argc>1 && !strcmp(argv[1],"rx_full");
    f.no_consumer=argc>1 && !strcmp(argv[1],"no_consumer");
    f.game_thread=GetCurrentThreadId();
    f.net.identity=remote; f.net.identity.player=101;
    f.net.stats.peers=1; f.net.stats.membership=1;
    f.opened=CreateEvent(NULL,TRUE,FALSE,NULL); f.release=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.initial=CreateEvent(NULL,TRUE,FALSE,NULL); f.tail_release=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.net.event=CreateEvent(NULL,FALSE,FALSE,NULL);
    CHECK(f.opened && f.release && f.initial && f.tail_release && f.net.event);
    memset(&config,0,sizeof config);
    config.signer=tagpu_recmp_key(); CHECK(config.signer);
    config.origin=11; config.count=4; config.net.match.Data1=1;
    config.net.host=1; config.net.address_bytes=3; memcpy(config.net.address,"ip!",3);
    for (i=0;i<4;i++) {
        config.actor[i].origin=i==0 ? 11 : i==1 ? 22 : i==2 ? 55 : 66;
        config.actor[i].local=i<2; tagpu_recmp_public(config.signer,&config.actor[i].public);
    }
    f.config=config;
    for (i=0;i<2;i++) {
        RecMpPacket* p;
        CHECK(tagpu_recmp_sign(config.signer,i ? 66 : 55,&stream,&remote,&binding));
        packet(i ? 66 : 55,RECMP_BIND,0,0,&binding,sizeof binding);
        p=packet(i ? 66 : 55,RECMP_RESOURCES,1,8,"first",5);
        CHECK(tagpu_recmp_stream_put(&f.source[i],&p->h,p->body,5));
    }
    if (f.rx_full) flood(0,55,8);
    journal=tagpu_recmp_record_begin(&config); CHECK(journal);
    c=tagpu_recmp_begin(&config); CHECK(c);
    CHECK(tagpu_recmp_record_attach(c,journal));
    CHECK(WaitForSingleObject(f.opened,15000)==WAIT_OBJECT_0);
    memset(&config,0,sizeof config); SetEvent(f.release);
    CHECK(WaitForSingleObject(f.initial,15000)==WAIT_OBJECT_0);
    CHECK(!tagpu_recmp_cut(c,55,&cut));
    if (f.rx_full) {
        /* The flood filled the queue before the game took anything: the game learns it at
           its first take and gives its recording up, as the recorder's mp_relinquish does */
        CHECK(tagpu_recmp_take_at(c,&got,100)==-2);
    } else {
        CHECK(tagpu_recmp_take_at(c,&got,100)==1 && got.h.origin==55);
        CHECK(tagpu_recmp_take_at(c,&got,100)==1 && got.h.origin==66);
    }
    if (f.relinquish || f.rx_full) {
        tagpu_recmp_relinquish(c);
        CHECK(tagpu_recmp_take_at(c,&got,100)==-2);
    }
    if (f.relinquish || f.rx_full || f.no_consumer) {
        tagpu_recmp_record_detach(c,journal);
        tagpu_recmp_record_end(journal);
    }
    if (f.journal_full) {
        RecMpHeader h=got.h;
        RecMpRecorded discarded;
        unsigned char body[RECMP_BODY_MAX]={0};
        h.origin=11; h.bytes=sizeof h+sizeof body;
        while (tagpu_recmp_record_put(journal,&h,body,sizeof body,100,RECMP_RECORD_PRODUCED)) {}
        CHECK(tagpu_recmp_record_take(journal,&discarded)==-1);
    }
    CHECK(tagpu_recmp_advance(c,100));
    CHECK(tagpu_recmp_send(c,RECMP_RESOURCES,100,"local",5));
    tagpu_recmp_retain(c); tagpu_recmp_end(c);
    tagpu_recmp_record_end(journal);
    if (f.relinquish || f.rx_full || f.no_consumer || f.journal_full) CHECK(!tagpu_recmp_cut(c,66,&cut));
    else CHECK(tagpu_recmp_cut(c,66,&frozen) && frozen.origin==66 && frozen.sequence==1 &&
               frozen.source_tick==8 && frozen.application_tick==100 && frozen.application_order==2 &&
               frozen.stream.Data1==2);
    CHECK(!tagpu_recmp_cut(c,11,&cut) && !tagpu_recmp_cut(c,999,&cut));
    /* A relinquished consumer's channel stops requiring cuts (tagpu_recmp.c terminal_ready).
       Releasing the proof tail only after the finish makes that an ordering rather than a race
       against the worker: no remote source has sealed, so the channel holds no cut, and the
       test shows that the finish does not wait for one. */
    if (f.relinquish) channel_finished(c,&stats);
    SetEvent(f.tail_release);
    channel_finished(c,&stats);
    if (f.missing_receipt || f.missing_cut || f.bad_receipt) {
        CHECK(f.aborted && stats.failed==(f.bad_receipt ? E_INVALIDARG : HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
        if (f.missing_receipt) CHECK(stats.cuts==2 && stats.receipts==3);
        if (f.missing_cut) CHECK(stats.cuts==1 && stats.receipts==4);
        puts("PASS: malformed/missing receipt or missing cut proof fails incomplete, even with zero pending native sends");
    } else if (f.relinquish || f.rx_full || f.no_consumer || f.journal_full) {
        /* Consumption ended, the channel did not: the local finals went out and every
           receipt came back, nothing was published after the end, and no cut is offered */
        CHECK(!f.aborted && !stats.failed && f.final_sent==2 && stats.receipts==4 &&
              !stats.pending && !stats.queued && !stats.lost);
        /* A failed journal detaches itself, so it consumes after stop no more than an absent one */
        CHECK(stats.consumption==(f.relinquish ? RECMP_RELINQUISHED : f.rx_full ? RECMP_RECEIVE_FULL :
                                  RECMP_NO_CONSUMER));
        CHECK(stats.unread==(f.rx_full ? 256u : 0u) && stats.received==(f.rx_full ? 0u : 2u));
        if (f.relinquish) CHECK(stats.cuts==0);
        CHECK(tagpu_recmp_take_at(c,&got,9999)==-2 && !tagpu_recmp_cut(c,55,&cut));
        printf("PASS: %s ends consumption, not the channel: final seals, receipts and a clean close "
               "continue%s\n", f.relinquish ? "a relinquishing consumer" : f.rx_full ?
               "a full receive queue" : f.journal_full ? "a stop after the journal failed" :
               "a stop with no recording attached",
               f.relinquish ? ", with no cut required" : "");
    } else {
        CHECK(!f.aborted && !stats.failed && stats.cuts==2 && stats.receipts==4 &&
              !stats.pending && !stats.queued && !stats.ended);
        while (tagpu_recmp_take_at(c,&got,9999)>0) drained++;
        CHECK(drained==3 && tagpu_recmp_cut(c,66,&cut) && !memcmp(&cut,&frozen,sizeof cut));
        puts("PASS: active peers need not end their match; exact consumption cuts, final receipts and retained post-teardown tail drain without changing the cut");
    }
    tagpu_recmp_release(c);
    {
        RecMpRecorded record;
        unsigned records=0;
        while (tagpu_recmp_record_take(journal,&record)>0) {
            CHECK(record.where.ordinal==++records && record.where.tick==100);
            CHECK(record.where.phase==(records<=2 ? RECMP_RECORD_CONSUMED : RECMP_RECORD_PRODUCED));
        }
        CHECK(records==(f.journal_full || f.rx_full ? 0u : f.relinquish || f.no_consumer ? 2u : 5u) &&
              tagpu_recmp_record_done(journal));
    }
    tagpu_recmp_record_free(journal);
    CloseHandle(f.opened); CloseHandle(f.release); CloseHandle(f.initial);
    CloseHandle(f.tail_release); CloseHandle(f.net.event);
    return 0;
}
