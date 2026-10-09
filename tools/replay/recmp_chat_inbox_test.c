#include "../../tagpu/ddraw/src/tagpu_recmp_chat_inbox.c"
#include "crc32.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static GUID match={ .Data1=1 };
static RecMpChatInbox* inbox;
static volatile unsigned clock_value;
static unsigned emitted, last_tick;
#define OFF_PLAYERS 0x1B63
#define PL_STRIDE 0x14B
#define REC_PLAYERS 10
#define OFF_NETWORK 0x2A44
#define OFF_GAMETIME 0x38A47
static char adapter_main[0x3924D];
static unsigned adapter_mode=3;
static RecMpChatInbox* s_mp_chat_inbox;
static DWORD span_tls;
static volatile LONG adapter_logs;
typedef struct { const void* pointer; unsigned bytes; } TestSpan;
static char* ta_main(void) { return adapter_main; }
static int game_mode(const char* main) { (void)main; return adapter_mode; }
static unsigned tagpu_wire_received(const void* pointer)
{
    const TestSpan* span=TlsGetValue(span_tls);
    return span && span->pointer==pointer ? span->bytes : 0;
}
static void tagpu_logf(const char* format, ...)
{
    (void)format; InterlockedIncrement(&adapter_logs);
}
#include "recmp-chat-receive.inc"
static RecMpRoster fixture_roster(void)
{
    RecMpRoster r={ .version=1, .count=10, .each=2, .slots=21 };
    for (unsigned i=0;i<10;i++) r.actor[i]=(RecMpSeat){11+11*i,i,1+2*i,2+2*i};
    return r;
}
static RecMpChatWire packet(uint64_t event)
{
    unsigned char stock[RECMP_CHAT_STOCK_BYTES];
    RecMpChatWire wire;
    stock[0]=5; memset(stock+1,'T',sizeof stock-1);
    CHECK(tagpu_recmp_chat_wire_make(&wire,&match,event,13,stock,sizeof stock));
    return wire;
}
static int collect(void* context,unsigned origin,unsigned tick,const void* body,unsigned bytes)
{
    unsigned count;
    (void)context;
    CHECK(origin==11 && (!emitted || tick>=last_tick));
    memcpy(&count,body,4);
    CHECK(bytes==4+count*sizeof(RecMpTextEntry));
    for (unsigned i=0;i<count;i++) {
        RecMpTextEntry e;
        memcpy(&e,(const char*)body+4+i*sizeof e,sizeof e);
        CHECK(e.recipient==11 && e.stock[0]==5 && e.stock[1]=='T');
        CHECK(e.source_tick==(e.phase==RECMP_TEXT_SOURCE ? tick : 13));
        emitted++;
    }
    last_tick=tick; return 1;
}
typedef struct { RecMpChatWire wire; HANDLE ready; unsigned accepted; } ClockCall;
static DWORD WINAPI clock_receiver(void* argument)
{
    ClockCall* call=argument;
    RecMpChatWire copied;
    SetEvent(call->ready);
    CHECK(tagpu_recmp_chat_inbox_receive(inbox,22,11,&call->wire,sizeof call->wire,
                                         &clock_value,&copied,&call->accepted));
    CHECK(!memcmp(&copied,&call->wire,sizeof copied));
    return 0;
}
static DWORD WINAPI closing_receiver(void* argument)
{
    ClockCall* call=argument;
    RecMpChatWire copied;
    unsigned tick;
    SetEvent(call->ready);
    call->accepted=tagpu_recmp_chat_inbox_receive(inbox,22,11,&call->wire,sizeof call->wire,
                                                 &clock_value,&copied,&tick);
    return 0;
}
static DWORD WINAPI receiver(void* argument)
{
    unsigned id=(unsigned)(size_t)argument;
    for (unsigned i=0;i<2000;i++) {
        RecMpChatWire wire=packet(i+1), copied;
        unsigned tick;
        CHECK(tagpu_recmp_chat_inbox_receive(inbox,id,11,&wire,sizeof wire,
                                             &clock_value,&copied,&tick));
        CHECK(tick==7 && !memcmp(&copied,&wire,sizeof wire));
    }
    return 0;
}
static RecMpTextQueue* claim_queue(const RecMpRoster* roster)
{
    GUID copied;
    DWORD host;
    RecMpTextQueue* q=tagpu_recmp_text_queue_begin();
    CHECK(q && tagpu_recmp_chat_inbox_claim(inbox,&copied,&host));
    CHECK(host==22 && !memcmp(&copied,&match,sizeof match));
    CHECK(!tagpu_recmp_chat_inbox_claim(inbox,&copied,&host));
    CHECK(tagpu_recmp_chat_inbox_drain(inbox,&match,11,roster,q));
    return q;
}
static int adapter_call(RecMpChatWire wire, unsigned span_bytes, unsigned row)
{
    unsigned char frame_storage[96]={0};
    unsigned char expected[RECMP_CHAT_STOCK_BYTES];
    void* frame=frame_storage+40;
    unsigned char* message=(unsigned char*)&wire;
    const char* source=adapter_main+OFF_PLAYERS+PL_STRIDE;
    const char* destination=adapter_main+OFF_PLAYERS;
    TestSpan span={message,span_bytes};
    memcpy(expected,wire.stock,sizeof expected);
    memcpy((char*)frame-0x24,&source,sizeof source);
    memcpy((char*)frame-0x14,&destination,sizeof destination);
    memcpy((char*)frame+0x10,&message,sizeof message);
    *((unsigned char*)frame+0x14)=(unsigned char)row;
    CHECK(TlsSetValue(span_tls,&span));
    int ok=mp_chat_receive(frame);
    CHECK(TlsSetValue(span_tls,NULL));
    if (ok) CHECK(!memcmp(message,expected,sizeof expected));
    else CHECK(message[0]==RECMP_CHAT_CODE);
    return ok;
}
static DWORD WINAPI loading_adapter(void* argument)
{
    (void)argument; CHECK(adapter_call(packet(102),sizeof(RecMpChatWire),1));
    return 0;
}
int main(void)
{
    RecMpRoster roster=fixture_roster();
    RecMpChatWire wire=packet(1), copied;
    RecMpTextQueue* q;
    GUID expected=match, other={ .Data1=2 };
    unsigned tick;
    inbox=tagpu_recmp_chat_inbox_begin(); CHECK(inbox);
    tagpu_recmp_chat_inbox_start(inbox,&expected,22); expected.Data1=99;
    CHECK(tagpu_recmp_chat_inbox_match(inbox,&expected) && expected.Data1==1);
    clock_value=0;
    CHECK(tagpu_recmp_chat_inbox_receive(inbox,22,11,&wire,sizeof wire,
                                         &clock_value,&copied,&tick) && !tick);
    memset(&wire,0,sizeof wire);
    RecMpTextEntry source={ .phase=RECMP_TEXT_SOURCE, .origin=11, .recipient=11,
                            .event=2, .source_tick=0, .display=4 };
    source.stock[0]=5; memset(source.stock+1,'T',sizeof source.stock-1);
    CHECK(tagpu_recmp_chat_inbox_source(inbox,&match,0,&source));
    memset(&source,0,sizeof source);
    tagpu_recmp_chat_inbox_level_end(inbox);
    q=claim_queue(&roster);
    CHECK(tagpu_recmp_text_queue_flush(q,0,collect,NULL) && emitted==2 && !last_tick);
    wire=packet(3); clock_value=1;
    CHECK(tagpu_recmp_chat_inbox_receive(inbox,22,11,&wire,sizeof wire,
                                         &clock_value,&copied,&tick) && tick==1);
    CHECK(!tagpu_recmp_chat_inbox_receive(inbox,22,11,&wire,sizeof wire-1,
                                          &clock_value,&copied,&tick));
    wire.match=other;
    CHECK(!tagpu_recmp_chat_inbox_receive(inbox,22,11,&wire,sizeof wire,
                                          &clock_value,&copied,&tick));
    CHECK(tagpu_recmp_chat_inbox_drain(inbox,&match,11,&roster,q));
    CHECK(tagpu_recmp_text_queue_flush(q,1,collect,NULL) && emitted==3 && last_tick==1);
    ClockCall closing={ .wire=packet(4), .ready=CreateEvent(NULL,TRUE,FALSE,NULL), .accepted=1 };
    CHECK(closing.ready);
    wire=packet(3);
    CHECK(tagpu_recmp_chat_inbox_receive(inbox,22,11,&wire,sizeof wire,
                                         &clock_value,&copied,&tick));
    EnterCriticalSection(&inbox->lock);
    HANDLE closing_thread=CreateThread(NULL,0,closing_receiver,&closing,0,NULL);
    CHECK(closing_thread && WaitForSingleObject(closing.ready,30000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(closing_thread,0)==WAIT_TIMEOUT);
    CHECK(tagpu_recmp_chat_inbox_finish(inbox,&match,11,&roster,q));
    CHECK(!tagpu_recmp_chat_inbox_match(inbox,&expected));
    CHECK(tagpu_recmp_text_queue_flush(q,1,collect,NULL) && emitted==4);
    tagpu_recmp_text_queue_end(q);
    LeaveCriticalSection(&inbox->lock);
    CHECK(WaitForSingleObject(closing_thread,30000)==WAIT_OBJECT_0 && !closing.accepted);
    CloseHandle(closing_thread); CloseHandle(closing.ready);
    source=(RecMpTextEntry){ .phase=RECMP_TEXT_SOURCE, .origin=11, .recipient=11,
                            .event=5, .source_tick=1, .display=4 };
    memcpy(source.stock,wire.stock,sizeof source.stock);
    CHECK(!tagpu_recmp_chat_inbox_source(inbox,&match,1,&source));
    tagpu_recmp_chat_inbox_level_end(inbox);
    CHECK(!tagpu_recmp_chat_inbox_match(inbox,&expected));

    tagpu_recmp_chat_inbox_start(inbox,&match,22);
    ClockCall call={ .wire=packet(4), .ready=CreateEvent(NULL,TRUE,FALSE,NULL) };
    CHECK(call.ready); clock_value=7;
    EnterCriticalSection(&inbox->lock);
    HANDLE thread=CreateThread(NULL,0,clock_receiver,&call,0,NULL);
    CHECK(thread && WaitForSingleObject(call.ready,30000)==WAIT_OBJECT_0);
    CHECK(WaitForSingleObject(thread,0)==WAIT_TIMEOUT);
    clock_value=11;
    LeaveCriticalSection(&inbox->lock);
    CHECK(WaitForSingleObject(thread,30000)==WAIT_OBJECT_0 && call.accepted==11);
    CloseHandle(thread); CloseHandle(call.ready);
    q=claim_queue(&roster);
    CHECK(tagpu_recmp_text_queue_flush(q,11,collect,NULL) && emitted==5 && last_tick==11);
    tagpu_recmp_text_queue_end(q);

    tagpu_recmp_chat_inbox_start(inbox,&match,22); clock_value=7;
    HANDLE workers[2]={CreateThread(NULL,0,receiver,(void*)(size_t)22,0,NULL),
                       CreateThread(NULL,0,receiver,(void*)(size_t)33,0,NULL)};
    CHECK(workers[0] && workers[1]);
    CHECK(WaitForMultipleObjects(2,workers,TRUE,30000)==WAIT_OBJECT_0);
    CloseHandle(workers[0]); CloseHandle(workers[1]);
    q=claim_queue(&roster); emitted=0;
    CHECK(tagpu_recmp_text_queue_flush(q,7,collect,NULL) && emitted==4000);
    tagpu_recmp_text_queue_end(q);

    tagpu_recmp_chat_inbox_start(inbox,&match,22); clock_value=0; wire=packet(5);
    for (unsigned i=0;i<=RECMP_TEXT_PENDING;i++)
        CHECK(tagpu_recmp_chat_inbox_receive(inbox,22,11,&wire,sizeof wire,
                                             &clock_value,&copied,&tick));
    GUID claimed; DWORD host;
    CHECK(tagpu_recmp_chat_inbox_claim(inbox,&claimed,&host));
    q=tagpu_recmp_text_queue_begin(); CHECK(q);
    CHECK(!tagpu_recmp_chat_inbox_drain(inbox,&match,11,&roster,q));
    tagpu_recmp_chat_inbox_start(inbox,&other,22);
    CHECK(!tagpu_recmp_chat_inbox_drain(inbox,&match,11,&roster,q));
    CHECK(tagpu_recmp_chat_inbox_claim(inbox,&claimed,&host) && claimed.Data1==2);
    wire=packet(6); wire.match=other;
    CHECK(tagpu_recmp_chat_inbox_receive(inbox,999,11,&wire,sizeof wire,
                                         &clock_value,&copied,&tick));
    CHECK(!tagpu_recmp_chat_inbox_drain(inbox,&other,11,&roster,q));
    tagpu_recmp_text_queue_end(q);

    tagpu_recmp_chat_inbox_start(inbox,&match,22);
    s_mp_chat_inbox=inbox; span_tls=TlsAlloc(); CHECK(span_tls!=TLS_OUT_OF_INDEXES);
    unsigned active=1, local=11, remote=22, state=5;
    memcpy(adapter_main+OFF_PLAYERS,&active,4);
    memcpy(adapter_main+OFF_PLAYERS+4,&local,4);
    memcpy(adapter_main+OFF_PLAYERS+PL_STRIDE,&active,4);
    memcpy(adapter_main+OFF_PLAYERS+PL_STRIDE+4,&remote,4);
    adapter_main[OFF_PLAYERS+0x73]=1;
    adapter_main[OFF_PLAYERS+PL_STRIDE+0x73]=3;
    adapter_main[OFF_NETWORK]=1;
    memcpy(adapter_main+0x391F1,&state,4);
    memcpy(adapter_main+0x4C9,&remote,4); memcpy(adapter_main+0x4CD,&local,4);
    CHECK(adapter_call(packet(101),sizeof(RecMpChatWire),1));
    thread=CreateThread(NULL,0,loading_adapter,NULL,0,NULL); CHECK(thread);
    CHECK(WaitForSingleObject(thread,30000)==WAIT_OBJECT_0); CloseHandle(thread);
    CHECK(adapter_logs==2);
    CHECK(!adapter_call(packet(103),sizeof(RecMpChatWire)-1,1));
    CHECK(!adapter_call(packet(103),sizeof(RecMpChatWire),0));
    adapter_main[OFF_NETWORK]=0;
    CHECK(!adapter_call(packet(103),sizeof(RecMpChatWire),1));
    adapter_main[OFF_NETWORK]=1; state=4;
    memcpy(adapter_main+0x391F1,&state,4);
    CHECK(!adapter_call(packet(103),sizeof(RecMpChatWire),1));
    state=5; memcpy(adapter_main+0x391F1,&state,4);
    adapter_mode=2;
    CHECK(!adapter_call(packet(103),sizeof(RecMpChatWire),1));
    adapter_mode=3; adapter_main[OFF_PLAYERS+PL_STRIDE+0x73]=2;
    CHECK(!adapter_call(packet(103),sizeof(RecMpChatWire),1));
    adapter_main[OFF_PLAYERS+PL_STRIDE+0x73]=3;
    tagpu_recmp_chat_inbox_level_end(inbox);
    q=claim_queue(&roster); emitted=0;
    CHECK(tagpu_recmp_text_queue_flush(q,0,collect,NULL) && emitted==2 && !last_tick);
    tagpu_recmp_text_queue_end(q); TlsFree(span_tls);
    tagpu_recmp_chat_inbox_end(inbox);
    puts("PASS copied loading inbox and exact production receive adapter: pending reset survival, once-only bootstrap, original zero/source clocks, bounded identity/packet validation, acceptance-clock publication ordering, final transfer closes admission before queue free, 4000 concurrent receipts, overflow without stock suppression, frozen-roster and changed-match refusal, state-5 game/foreign-thread acceptance without a producer, stock packet restoration and mode/state/row/span/controller/network refusal");
    return 0;
}
