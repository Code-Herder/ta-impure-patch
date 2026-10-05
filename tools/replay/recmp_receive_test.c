#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define OFF_NETWORK 0x2A44
#define CHECK(x) do { if (!(x)) { printf("FAIL %u: %s\n",__LINE__,#x); exit(1); } } while (0)
static char s_test_main[0x3924D];
static unsigned s_test_queued=1, s_test_size=65, s_test_source=11, s_test_calls;
static HRESULT s_test_error;
static unsigned char s_test_stock[128];
static char* ta_main(void) { return s_test_main; }
#define tagpu_logf(...) printf(__VA_ARGS__)
static HRESULT __stdcall raw_receive(void* context,void* buffer,DWORD* bytes)
{
    CHECK(context==s_test_main+0x14);
    s_test_calls++;
    if (s_test_error) { *bytes=s_test_size; return s_test_error; }
    if (*bytes<s_test_size) { *bytes=s_test_size; return (HRESULT)0x8877001E; }
    memset(buffer,(unsigned char)s_test_source,s_test_size);
    *(unsigned char*)buffer=0x2C;
    *bytes=s_test_size;
    memcpy(s_test_main+0x4C9,&s_test_source,4);
    unsigned to=s_test_source+100;
    memcpy(s_test_main+0x4CD,&to,4);
    return S_OK;
}
static HRESULT __attribute__((thiscall)) queued_receive(void* self,void* context,void* buffer,DWORD* bytes)
{
    CHECK(self==(void*)0x51E300 && *bytes>=65536);
    return raw_receive(context,buffer,bytes);
}
static void __stdcall message_stats(unsigned code,unsigned bytes,unsigned sending)
{ CHECK(code==0x2C && bytes==s_test_size && !sending); }
static void __stdcall packet_stats(unsigned bytes,unsigned queued,unsigned sending)
{ CHECK(bytes==s_test_size && !queued && !sending); }
enum { PR_EDI, PR_ESI, PR_EBP, PR_ESP, PR_EBX, PR_EDX, PR_ECX, PR_EAX, PR_RET };
#define WPN_ESP_JMP(r) ((unsigned char*)&(r)[PR_RET])
enum { WIRE_2C_MAX=0xFFFF, WIRE_2C_PAD=48 };
static unsigned char s_wire2cBuf[WIRE_2C_MAX+WIRE_2C_PAD];
static const unsigned char* s_wire2cRd;
static const unsigned char* s_wire2cCopy;
static unsigned long long s_wire2cEnd;
static DWORD s_wireTlsBuf=TLS_OUT_OF_INDEXES, s_wireTlsLen=TLS_OUT_OF_INDEXES;
static struct { unsigned drop2CStale; } s_wireDiag;
static void wire_drop(unsigned* count,const char* text,unsigned a,unsigned b)
{ (void)text; (void)a; (void)b; (*count)++; }
#include "recmp-receive-core.inc"
static void movement_copy(void* message,unsigned bytes,int accepted)
{
    unsigned frame[16]={0};
    frame[PR_EAX]=(unsigned)(size_t)message;
    CHECK(wire_s2c_copy(frame)==accepted);
    if (!accepted) { CHECK(!s_wire2cCopy && !s_wire2cRd); return; }
    CHECK(s_wire2cCopy==s_wire2cBuf && s_wire2cRd==WPN_ESP_JMP(frame)+0x10);
    CHECK(s_wire2cEnd==(unsigned long long)bytes*8 && frame[PR_EAX]==(unsigned)(size_t)s_wire2cBuf);
    CHECK(!memcmp(message,s_wire2cBuf,bytes));
    for (unsigned i=0;i<WIRE_2C_PAD;i++) CHECK(!s_wire2cBuf[bytes+i]);
}
static void valid(MpReceiveFrame* frame,unsigned source,unsigned size)
{
    unsigned entry[6]={0};
    s_test_source=source; s_test_size=size; s_test_error=0;
    CHECK(mp_receive_packet(entry));
    CHECK((unsigned)(size_t)frame->buffer==entry[5] && frame->bytes==size && frame->capacity>=size);
    CHECK(tagpu_rec_wire_received(frame->buffer)==size);
    CHECK(!tagpu_rec_wire_received(frame->buffer+1) && !tagpu_rec_wire_received(s_test_stock));
    memset(s_test_stock,0xEE,sizeof s_test_stock);
    CHECK(frame->buffer[0]==0x2C);
    for (unsigned i=1;i<size;i++) CHECK(frame->buffer[i]==(unsigned char)source);
}
static DWORD WINAPI worker(void* argument)
{
    unsigned id=(unsigned)(size_t)argument;
    for (unsigned i=0;i<2000;i++) {
        MpReceiveFrame parent, child;
        CHECK(mp_receive_enter(&parent));
        valid(&parent,id,65);
        if (!(i%20)) {
            CHECK(mp_receive_enter(&child)); valid(&child,id+20,97); mp_receive_leave(&child);
            unsigned from,to;
            memcpy(&from,s_test_main+0x4C9,4); memcpy(&to,s_test_main+0x4CD,4);
            CHECK(from==id && to==id+100 && tagpu_rec_wire_received(parent.buffer)==65);
            CHECK(parent.buffer[1]==(unsigned char)id);
        }
        mp_receive_leave(&parent);
        CHECK(!tagpu_rec_wire_received(parent.buffer));
    }
    return 0;
}
int main(void)
{
    s_test_main[OFF_NETWORK]=1;
    void* stock=s_test_stock;
    memcpy(s_test_main+0x2A38,&stock,sizeof stock);
    s_mp_rx_tls=TlsAlloc(); CHECK(s_mp_rx_tls!=TLS_OUT_OF_INDEXES);
    s_wireTlsBuf=TlsAlloc(); s_wireTlsLen=TlsAlloc();
    CHECK(s_wireTlsBuf!=TLS_OUT_OF_INDEXES && s_wireTlsLen!=TLS_OUT_OF_INDEXES);
    CHECK(TlsSetValue(s_wireTlsBuf,s_test_stock) && TlsSetValue(s_wireTlsLen,(void*)65));
    CHECK(InitializeCriticalSectionAndSpinCount(&s_mp_rx_lock,0));
    MpReceiveFrame parent,child;
    CHECK(mp_receive_enter(&parent)); valid(&parent,11,65535);
    unsigned short full=65535; memcpy(parent.buffer+1,&full,2);
    movement_copy(parent.buffer,65535,1);
    movement_copy(parent.buffer+1,0,0);
    CHECK(tagpu_wire_received(parent.buffer)==65535);
    CHECK(mp_receive_enter(&child)); s_test_queued=0; s_test_calls=0; valid(&child,55,70001);
    CHECK(s_test_calls==2 && child.capacity==70001);
    memcpy(child.buffer+1,&full,2); movement_copy(child.buffer,65535,1);
    movement_copy(parent.buffer,0,0);
    mp_receive_leave(&child);
    CHECK(tagpu_rec_wire_received(parent.buffer)==65535 && parent.buffer[3]==11);
    unsigned short short_span=17; memcpy(parent.buffer+1,&short_span,2);
    movement_copy(parent.buffer,17,1);
    unsigned from,to; memcpy(&from,s_test_main+0x4C9,4); memcpy(&to,s_test_main+0x4CD,4);
    CHECK(from==11 && to==111);
    unsigned entry[6]={0};
    s_test_error=(HRESULT)0x8877001E; s_test_size=parent.capacity; s_test_calls=0;
    CHECK(!mp_receive_packet(entry) && !parent.bytes && s_test_calls==1 && !entry[5]);
    s_test_error=(HRESULT)0x887700BE;
    CHECK(!mp_receive_packet(entry) && !parent.bytes);
    s_test_main[OFF_NETWORK]=0; s_test_calls=0;
    CHECK(!mp_receive_packet(entry) && !s_test_calls);
    s_test_main[OFF_NETWORK]=1; mp_receive_leave(&parent);
    CHECK(!tagpu_rec_wire_received(parent.buffer));
    movement_copy(parent.buffer,0,0);
    s_test_stock[0]=0x2C; memcpy(s_test_stock+1,&full,2);
    movement_copy(s_test_stock,65,1);
    movement_copy(s_test_stock+1,0,0);
    s_test_queued=1;
    HANDLE threads[2]={CreateThread(NULL,0,worker,(void*)11,0,NULL),CreateThread(NULL,0,worker,(void*)55,0,NULL)};
    CHECK(threads[0] && threads[1]);
    CHECK(WaitForMultipleObjects(2,threads,TRUE,30000)==WAIT_OBJECT_0);
    CloseHandle(threads[0]); CloseHandle(threads[1]);
    DeleteCriticalSection(&s_mp_rx_lock); TlsFree(s_mp_rx_tls); s_mp_rx_tls=TLS_OUT_OF_INDEXES;
    TlsFree(s_wireTlsBuf); TlsFree(s_wireTlsLen);
    puts("PASS exact production receive core and movement copy: u16 maximum, raw growth, native send independence, nested lifetime/metadata/span, owned and legacy spans, header clamp and padding, stale/interior/closed-frame refusal, invalid size/error/network refusal, 4000 two-thread receives and 200 nested receives");
    return 0;
}
