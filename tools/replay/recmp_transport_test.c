/* Exercise the private channel with controlled native-backend boundaries.
   Include the real identity checks; replace only session open/send/receive/close. */
#define INITGUID
#define COBJMACROS
#include "tagpu_recmp_channel.h"
#include "tagpu_recmp_stream.h"
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

static struct {
    RecMpNet net;
    RecMpConfig expected;
    HANDLE opened, open_release, entered, send_release, closed, drained;
    RecMpPacket frame[16];
    DWORD from[16];
    unsigned count, read, announcements, data_sends, aborted, image_ok;
    DWORD game_thread;
} f;

void tagpu_logf(const char* fmt, ...)
{
    va_list args;
    va_start(args,fmt);
    vprintf(fmt,args);
    va_end(args);
    putchar('\n');
}

HRESULT tagpu_recmp_net_open(const RecMpNetConfig* config, RecMpNet** out)
{
    CHECK(GetCurrentThreadId() != f.game_thread);
    SetEvent(f.opened);
    CHECK(WaitForSingleObject(f.open_release,15000) == WAIT_OBJECT_0);
    CHECK(!memcmp(config,&f.expected.net,sizeof *config));
    *out = &f.net;
    return DP_OK;
}

HRESULT tagpu_recmp_net_send(RecMpNet* net, const void* bytes, unsigned size)
{
    const RecMpPacket* p = bytes;
    CHECK(net == &f.net && GetCurrentThreadId() != f.game_thread);
    CHECK(size == p->h.bytes && !memcmp(&p->h.match,&f.expected.net.match,sizeof p->h.match));
    if (p->h.kind == RECMP_BIND) {
        RecMpSignedIdentity checked;
        RecMpIdentity remote = net->identity;
        remote.player = 202;
        CHECK(!p->h.sequence && !p->h.tick && size == sizeof p->h + sizeof checked);
        CHECK(tagpu_recmp_verify(&f.expected.actor[0].public,p->h.origin,&p->h.stream,
                                  &remote,net->identity.player,p->body,sizeof checked,&checked));
        CHECK(p->h.origin == 11 || p->h.origin == 22);
        f.announcements |= p->h.origin == 11 ? 1 : 2;
    } else {
        CHECK(f.announcements == 3);
        CHECK(!f.data_sends);
        SetEvent(f.entered);
        CHECK(WaitForSingleObject(f.send_release,15000) == WAIT_OBJECT_0);
        f.image_ok = p->h.origin == 11 && p->h.tick == 9 && p->h.sequence == 1 &&
                     size == sizeof p->h + 8 && !memcmp(p->body,"retained",8);
        CHECK(f.image_ok);
        f.data_sends++;
    }
    net->stats.completed++;
    net->stats.completed_bytes += size;
    return DP_OK;
}

HRESULT tagpu_recmp_net_receive(RecMpNet* net, DWORD* from, void* packet, unsigned* bytes)
{
    unsigned at = f.read;
    CHECK(net == &f.net && GetCurrentThreadId() != f.game_thread);
    if (at == f.count) { SetEvent(f.drained); return DPERR_NOMESSAGES; }
    CHECK(*bytes >= f.frame[at].h.bytes);
    memcpy(packet,&f.frame[at],f.frame[at].h.bytes);
    *bytes = f.frame[at].h.bytes;
    *from = f.from[at];
    f.read++;
    return DP_OK;
}

HRESULT tagpu_recmp_net_close(RecMpNet* net)
{
    CHECK(net == &f.net && !net->stats.pending);
    SetEvent(f.closed);
    return DP_OK;
}

void tagpu_recmp_net_abort(RecMpNet* net)
{
    CHECK(net == &f.net && GetCurrentThreadId() != f.game_thread);
    f.aborted = 1;
    SetEvent(f.closed);
}

static void start_test(void)
{
    RecMpStart start = { .code=8,.version=RECMP_VERSION,.bytes=sizeof start,.origin=55,
                        .match={.Data1=1} };
    GUID match = {.Data1=9};
    unsigned char* pages = VirtualAlloc(NULL,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    void* at;
    DWORD old;
    CHECK(pages && VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old));
    at = pages+4096-sizeof start;
    memcpy(at,&start,sizeof start);
    CHECK(tagpu_recmp_start_read(at,sizeof start,55,&match) && match.Data1 == 1);
    CHECK(!tagpu_recmp_start_read(pages+4096,sizeof start-1,55,&match));
    CHECK(!tagpu_recmp_start_read(at,sizeof start+1,55,&match));
    CHECK(!tagpu_recmp_start_read(at,sizeof start,56,&match));
    CHECK(!tagpu_recmp_start_read(at,sizeof start,0,&match));
    start.code++; memcpy(at,&start,sizeof start);
    CHECK(!tagpu_recmp_start_read(at,sizeof start,55,&match));
    start.code=8; start.version++; memcpy(at,&start,sizeof start);
    CHECK(!tagpu_recmp_start_read(at,sizeof start,55,&match));
    start.version=RECMP_VERSION; start.bytes--; memcpy(at,&start,sizeof start);
    CHECK(!tagpu_recmp_start_read(at,sizeof start,55,&match));
    start.bytes=sizeof start; start.match.Data1=0; memcpy(at,&start,sizeof start);
    CHECK(!tagpu_recmp_start_read(at,sizeof start,55,&match) && match.Data1 == 1);
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
}

static RecMpPacket* frame(unsigned origin, unsigned kind, unsigned sequence,
                            unsigned tick, const void* body, unsigned bytes)
{
    RecMpPacket* p = &f.frame[f.count];
    CHECK(f.count < 16);
    f.from[f.count++] = 202;
    memset(p,0,sizeof *p);
    memcpy(p->h.magic,"IMPUREMP",8);
    p->h.version = RECMP_VERSION; p->h.header = sizeof p->h;
    p->h.bytes = sizeof p->h+bytes;
    p->h.origin = origin; p->h.kind = kind;
    p->h.sequence = sequence; p->h.tick = tick;
    p->h.match = f.expected.net.match; p->h.stream.Data1 = 2;
    if (bytes) memcpy(p->body,body,bytes);
    return p;
}

int main(int argc, char** argv)
{
    RecMpConfig config, bad;
    RecMpChannel* c;
    RecMpPublic public;
    RecMpIdentity remote = {RECMP_VERSION,sizeof remote,{.Data1=1},{.Data1=3},202};
    RecMpSignedIdentity binding;
    RecMpPacket got;
    RecMpHeader ai;
    RecMpStats stats;
    GUID stream = {.Data1=2};
    unsigned i, n;
    int gap = argc>1 && !strcmp(argv[1],"gap");
    int wrong = argc>1 && !strcmp(argv[1],"unauthorized");
    int tail = argc>1 && !strcmp(argv[1],"tail");
    int missing_end = argc>1 && !strcmp(argv[1],"missing_end");
    start_test();
    memset(&f,0,sizeof f);
    f.game_thread = GetCurrentThreadId();
    f.net.identity = remote; f.net.identity.player = 101;
    f.net.stats.peers=missing_end ? 0 : 1; f.net.stats.membership=1;
    f.opened=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.open_release=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.entered=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.send_release=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.closed=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.drained=CreateEvent(NULL,TRUE,FALSE,NULL);
    f.net.event=CreateEvent(NULL,FALSE,FALSE,NULL);
    CHECK(f.opened && f.open_release && f.entered && f.send_release && f.closed && f.drained && f.net.event);
    memset(&config,0,sizeof config);
    config.signer=tagpu_recmp_key(); CHECK(config.signer);
    tagpu_recmp_public(config.signer,&public);
    config.net.match.Data1=1; config.net.host=1;
    config.net.address_bytes=3; memcpy(config.net.address,"ip!",3);
    config.origin=11; config.count=4;
    for (i=0;i<4;i++) {
        config.actor[i].origin = i==0 ? 11 : i==1 ? 22 : i==2 ? 55 : 66;
        config.actor[i].local = i<2;
        config.actor[i].public=public;
    }
    f.expected=config;
    frame(55,RECMP_RESOURCES,32,8,"old",3)->h.match.Data1++;
    for (i=0;i<2;i++) {
        CHECK(tagpu_recmp_sign(config.signer,i ? 66 : 55,&stream,&remote,&binding));
        frame(i ? 66 : 55,RECMP_BIND,0,0,&binding,sizeof binding);
    }
    if (wrong) f.from[1]=203;
    frame(55,RECMP_RESOURCES,gap ? 3 : 1,8,"peer",4);
    frame(55,RECMP_RESOURCES,1,8,"peer",4);
    frame(66,RECMP_RESOURCES,1,8,"ai",2);
    if (tail) {
        RecMpStream source={0};
        RecMpSeal seal;
        RecMpPacket* p=&f.frame[3];
        CHECK(tagpu_recmp_stream_put(&source,&p->h,p->body,p->h.bytes-sizeof p->h));
        p=frame(55,RECMP_RESOURCES,2,8,"tail",4);
        CHECK(tagpu_recmp_stream_put(&source,&p->h,p->body,p->h.bytes-sizeof p->h));
        f.count--;                             /* withhold the last DATA, deliver its valid final seal */
        CHECK(tagpu_recmp_stream_seal(&source,8,1,&seal));
        frame(55,RECMP_SEAL,3,8,&seal,sizeof seal);
    }
    bad=config; bad.count=11; CHECK(!tagpu_recmp_begin(&bad));
    bad=config; bad.net.match.Data1=0; CHECK(!tagpu_recmp_begin(&bad));
    bad=config; bad.actor[1].origin=11; CHECK(!tagpu_recmp_begin(&bad));
    bad=config; bad.actor[0].local=0; CHECK(!tagpu_recmp_begin(&bad));
    bad=config; bad.actor[0].public.key[64]^=1; CHECK(!tagpu_recmp_begin(&bad));
    c=tagpu_recmp_begin(&config); CHECK(c);
    CHECK(WaitForSingleObject(f.opened,15000) == WAIT_OBJECT_0);
    CHECK(!tagpu_recmp_begin(&config));
    CHECK(tagpu_recmp_send(c,RECMP_RESOURCES,9,"retained",8));
    memset(&config,0,sizeof config);           /* begin owns its copy */
    SetEvent(f.open_release);
    if (missing_end) {
        CHECK(WaitForSingleObject(f.drained,15000) == WAIT_OBJECT_0);
        tagpu_recmp_stats(c,&stats);
        CHECK(stats.bound == 2 && !stats.ended && !stats.failed);
        tagpu_recmp_end(c);
        CHECK(WaitForSingleObject(f.closed,45000) == WAIT_OBJECT_0);
        CHECK(f.aborted && !f.data_sends);
        puts("PASS: absent unsealed source cannot retire queued data; deadline aborts incomplete without retaining the worker forever");
    } else if (gap || wrong || tail) {
        CHECK(WaitForSingleObject(f.closed,15000) == WAIT_OBJECT_0);
        tagpu_recmp_stats(c,&stats);
        CHECK(stats.failed == (wrong ? E_ACCESSDENIED : E_INVALIDARG));
        CHECK(!stats.received && !stats.sent && tagpu_recmp_take(c,&got) == -1 && f.aborted);
        tagpu_recmp_end(c);
        printf("PASS: %s ends the isolated contribution channel incomplete\n",
               wrong ? "wrong actual private sender" : tail ? "missing last DATA before a final seal" :
               "missing initial source sequence");
    } else {
        CHECK(WaitForSingleObject(f.entered,15000) == WAIT_OBJECT_0);
        CHECK(tagpu_recmp_take(c,&got) == 1 && got.h.origin == 55 && got.h.sequence == 1 &&
              !memcmp(got.body,"peer",4));
        CHECK(tagpu_recmp_take(c,&got) == 1 && got.h.origin == 66 && got.h.sequence == 1 &&
              !memcmp(got.body,"ai",2));
        CHECK(!tagpu_recmp_take(c,&got));
        CHECK(tagpu_recmp_send_from(c,22,RECMP_RESOURCES,9,"retained",8,&ai));
        CHECK(ai.origin == 22 && ai.sequence == 1);
        CHECK(tagpu_recmp_send_from(c,22,RECMP_RESOURCES,9,"retained",8,&ai));
        CHECK(ai.origin == 22 && ai.sequence == 2);
        for (n=0;n<256;n++) if (!tagpu_recmp_send(c,RECMP_RESOURCES,10,"retained",8)) break;
        CHECK(n == 253);
        tagpu_recmp_stats(c,&stats);
        CHECK(stats.queued == 256 && stats.received == 2 && stats.bound == 2 &&
              stats.expected == 2 && stats.failed == E_OUTOFMEMORY);
        tagpu_recmp_end(c);
        CHECK(!tagpu_recmp_begin(&f.expected));
        CHECK(WaitForSingleObject(f.closed,0) == WAIT_TIMEOUT);
        SetEvent(f.send_release);
        CHECK(WaitForSingleObject(f.closed,15000) == WAIT_OBJECT_0);
        CHECK(f.image_ok && f.data_sends == 1 && f.aborted);
        puts("PASS: copied roster/address, signed human/AI bindings before data, old-match and duplicate collapse, independent source sequences, bounded queue failure, retained packet through detached end");
    }
    /* Backend closure ends every path to these fixture handles. The channel's
       own wake handle instead lives through its final game/worker reference. */
    CloseHandle(f.opened); CloseHandle(f.open_release); CloseHandle(f.entered);
    CloseHandle(f.send_release); CloseHandle(f.closed); CloseHandle(f.drained); CloseHandle(f.net.event);
    return 0;
}
