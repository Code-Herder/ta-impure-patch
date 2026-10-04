/* Native TCP/IP fixture for the recorder's isolated transport. Run a host and
   joiner on the same private DirectPlay discovery port, passing a release-file
   path to each. It stands in for TA's primary session: identities travel from
   native player data for real human/AI IDs, before both primary interfaces close. */
#define INITGUID
#define COBJMACROS
#define _WIN32_WINNT 0x0400
#include "tagpu_recmp_channel.h"
#include "tagpu_log.h"
#include <stdarg.h>
#include <dplobby.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID s_game = {0x8d160301,0x4589,0x40ba,{0xa2,0x67,0x23,0x5a,0x1d,0x70,0x36,0x01}};
static const GUID s_match = {0x4df148d3,0xe0c0,0x4a8e,{0x85,0x27,0x36,0x6b,0x9e,0x7c,0x83,0x09}};
#define CHECK(x) do { if (!(x)) { printf("FAIL line %u: %s\n", __LINE__, #x); fflush(stdout); exit(1); } } while (0)

typedef struct {
    RecMpNetConfig config;
    DWORD local[2];
    struct { DWORD origin; RecMpPublic public; } remote[2];
    const RecMpKey* key;
    RecMpIdentity identity;
    volatile LONG ready, remote_count, closed;
    const char* release;
} Probe;

static unsigned atomic(volatile LONG* value) { return (unsigned)InterlockedCompareExchange(value, 0, 0); }

static void identity_bounds(void)
{
    unsigned char* pages = VirtualAlloc(NULL, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old;
    RecMpIdentity identity = {1,sizeof identity,{0},{0},19}, out = {0}, before;
    void* at;
    CHECK(pages && VirtualProtect(pages+4096,4096,PAGE_NOACCESS,&old));
    identity.match = s_match;
    identity.session.Data1 = 1;
    at = pages + 4096 - sizeof identity;
    memcpy(at, &identity, sizeof identity);
    CHECK(tagpu_recmp_identity_read(at,sizeof identity,&s_match,&out));
    before = out;
    CHECK(!tagpu_recmp_identity_read(pages+4096,sizeof identity-1,&s_match,&out));
    CHECK(!tagpu_recmp_identity_read(at,sizeof identity+1,&s_match,&out));
    identity.version++;
    memcpy(at,&identity,sizeof identity);
    CHECK(!tagpu_recmp_identity_read(at,sizeof identity,&s_match,&out));
    identity.version = 1;
    identity.match.Data1++;
    memcpy(at,&identity,sizeof identity);
    CHECK(!tagpu_recmp_identity_read(at,sizeof identity,&s_match,&out));
    identity.match = s_match;
    identity.player = 0;
    memcpy(at,&identity,sizeof identity);
    CHECK(!tagpu_recmp_identity_read(at,sizeof identity,&s_match,&out));
    identity.player = 19;
    identity.session.Data1 = 0;
    memcpy(at,&identity,sizeof identity);
    CHECK(!tagpu_recmp_identity_read(at,sizeof identity,&s_match,&out));
    CHECK(!memcmp(&out,&before,sizeof out));
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
}

static HRESULT create(HMODULE module, const CLSID* clsid, const IID* iid, void** out)
{
    typedef HRESULT (WINAPI *GetClass)(const CLSID*, const IID*, void**);
    GetClass get_class = (GetClass)GetProcAddress(module,"DllGetClassObject");
    IClassFactory* factory = NULL;
    HRESULT hr;
    CHECK(get_class);
    hr = get_class(clsid,&IID_IClassFactory,(void**)&factory);
    if (FAILED(hr)) return hr;
    hr = IClassFactory_CreateInstance(factory,NULL,iid,out);
    IClassFactory_Release(factory);
    return hr;
}

typedef struct { GUID guid; DWORD host; } Session;
static BOOL WINAPI found(LPCDPSESSIONDESC2 desc, LPDWORD timeout, DWORD flags, void* arg)
{
    Session* session = arg;
    (void)timeout;
    if (flags & DPESC_TIMEDOUT) return FALSE;
    session->guid = desc->guidInstance;
    session->host = desc->dwUser1;
    return FALSE;
}

#ifdef RECMP_CHANNEL_TEST
#include "tagpu_recmp_record.h"
#include "tagpu_recmp_stream.h"
static HANDLE s_finished;

void tagpu_logf(const char* fmt, ...)
{
    va_list args;
    va_start(args,fmt);
    vprintf(fmt,args);
    va_end(args);
    putchar('\n');
    if (strstr(fmt,"isolated worker ended:")) SetEvent(s_finished);
}

static void channel_run(Probe* p)
{
    RecMpConfig config = {0};
    RecMpChannel* channel;
    RecMpJournal* journal;
    RecMpPublic public;
    RecMpStats stats;
    RecMpPacket packet;
    unsigned sequences[2] = {0}, received = 0, seals = 0, finals = 0, i, sent;
    char send_release[MAX_PATH];
    DWORD deadline = GetTickCount()+45000;
    config.net = p->config;
    memset(&config.net.provider,0,sizeof config.net.provider);
    config.origin = p->local[0];
    config.signer = p->key;
    config.count = 4;
    tagpu_recmp_public(p->key,&public);
    for (i=0;i<2;i++) {
        config.actor[i].origin = p->local[i];
        config.actor[i].local = 1;
        config.actor[i].public = public;
        config.actor[i+2].origin = p->remote[i].origin;
        config.actor[i+2].public = p->remote[i].public;
    }
    strcpy(send_release,p->release); strcat(send_release,".send");
    while (GetFileAttributesA(send_release) == INVALID_FILE_ATTRIBUTES) {
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        Sleep(1);
    }
    CHECK(atomic(&p->closed));
    s_finished = CreateEvent(NULL,TRUE,FALSE,NULL); CHECK(s_finished);
    journal = tagpu_recmp_record_begin(&config); CHECK(journal);
    channel = tagpu_recmp_begin(&config); CHECK(channel);
    CHECK(tagpu_recmp_record_attach(channel,journal));
    CHECK(!tagpu_recmp_record_attach(channel,journal));
    memset(&config,0,sizeof config);
    CHECK(tagpu_recmp_advance(channel,1));
    CHECK(tagpu_recmp_advance(channel,1));
    for (sent=0;sent<16;sent++) {
        unsigned char body[16];
        memset(body,(int)(sent/2+1),sizeof body);
        CHECK(tagpu_recmp_send_from(channel,p->local[sent%2],RECMP_RESOURCES,1,
                                     body,sizeof body,NULL));
        memset(body,0xEE,sizeof body);
    }
    CHECK(tagpu_recmp_advance(channel,2));
    CHECK(tagpu_recmp_advance(channel,2));
    CHECK(tagpu_recmp_finish_sources(channel));
    CHECK(tagpu_recmp_finish_sources(channel));
    printf("CHANNEL QUEUED: 16 data packets and human/AI prefix seals after primary Close\n");
    for (;;) {
        int got;
        while ((got=tagpu_recmp_take_at(channel,&packet,2)) > 0) {
            unsigned actor;
            for (actor=0;actor<2;actor++) if (p->remote[actor].origin == packet.h.origin) break;
            CHECK(actor < 2 && packet.h.sequence == ++sequences[actor]);
            if (packet.h.kind == RECMP_SEAL) {
                RecMpSeal seal;
                CHECK(packet.h.bytes == sizeof packet.h+sizeof seal);
                memcpy(&seal,packet.body,sizeof seal);
                CHECK(seal.previous == packet.h.sequence-1 && seal.tick == packet.h.tick &&
                      seal.data_bytes == 8*(sizeof packet.h+16));
                if (seal.final) { CHECK(packet.h.sequence == 10 && seal.tick == 2); finals++; }
                else CHECK(packet.h.sequence == 9 && seal.tick == 1);
                seals++;
            } else {
                CHECK(packet.h.kind == RECMP_RESOURCES && packet.h.bytes == sizeof packet.h+16);
                for (i=0;i<16;i++) CHECK(packet.body[i] == packet.h.sequence);
                received++;
            }
        }
        CHECK(got == 0);
        tagpu_recmp_stats(channel,&stats);
        CHECK(!stats.failed && !stats.lost && (LONG)(deadline-GetTickCount()) > 0);
        if (received == 16 && finals == 2 && !stats.queued && !stats.pending &&
            stats.receipts == 4 && stats.receipts_sent == 4) break;
        Sleep(1);
    }
    CHECK(stats.bound == 2 && stats.expected == 2 && stats.player &&
          stats.sent == 20 && stats.bytes == 16*(sizeof(RecMpHeader)+16)+4*(sizeof(RecMpHeader)+sizeof(RecMpSeal)) &&
          stats.received == 20 && sequences[0] == 10 && sequences[1] == 10 &&
          seals == 4 && stats.seals == 4 && stats.ended == 2);
    CHECK(stats.completed_bytes == stats.bytes +
          stats.receipts_sent*(sizeof(RecMpHeader)+sizeof(RecMpReceipt)) +
          (stats.completed-20-stats.receipts_sent)*(sizeof(RecMpHeader)+sizeof(RecMpSignedIdentity)));
    printf("PASS: production channel after primary Close, signed human/AI bindings, "
           "16 data deliveries, exact human/AI final prefixes, %u successful completions / %u bytes, pending zero\n",
           stats.completed,stats.completed_bytes);
    while (GetFileAttributesA(p->release) == INVALID_FILE_ATTRIBUTES) {
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        Sleep(1);
    }
    tagpu_recmp_end(channel);
    tagpu_recmp_record_end(journal);
    {
        RecMpRecorded record;
        RecMpStream ledger[4]={{0}};
        const RecMpRecordHeader* rh=tagpu_recmp_record_header(journal);
        char path[MAX_PATH];
        FILE* file;
        unsigned ordinal=0;
        CHECK(strlen(p->release)+sizeof ".host.journal" <= sizeof path);
        strcpy(path,p->release); strcat(path,p->config.host ? ".host.journal" : ".join.journal");
        file=fopen(path,"wb"); CHECK(file);
        CHECK(fwrite(rh,sizeof *rh,1,file)==1);
        while (tagpu_recmp_record_take(journal,&record)>0) {
            unsigned actor;
            CHECK(record.where.ordinal==++ordinal);
            for (actor=0;actor<rh->count;actor++) if (rh->actor[actor].origin==record.packet.h.origin) break;
            CHECK(actor<rh->count && record.where.phase==
                  (rh->actor[actor].local ? RECMP_RECORD_PRODUCED : RECMP_RECORD_CONSUMED));
            CHECK(tagpu_recmp_stream_put(&ledger[actor],&record.packet.h,record.packet.body,
                                         record.packet.h.bytes-sizeof record.packet.h));
            CHECK(fwrite(&record.where,sizeof record.where,1,file)==1 &&
                  fwrite(&record.packet,record.packet.h.bytes,1,file)==1);
        }
        CHECK(ordinal==40 && tagpu_recmp_record_done(journal));
        for (i=0;i<4;i++) CHECK(ledger[i].ended && ledger[i].sequence==10);
        CHECK(!fclose(file));
        printf("PASS: persisted 40 ordered human/AI produced/consumed packets with independently validated source ledgers\n");
    }
    tagpu_recmp_record_free(journal);
    CHECK(WaitForSingleObject(s_finished,15000) == WAIT_OBJECT_0);
    CloseHandle(s_finished);
}
#else
static void header(Probe* p, RecMpHeader* h, unsigned actor, unsigned kind,
                    unsigned sequence, unsigned bytes)
{
    memset(h,0,sizeof *h);
    memcpy(h->magic,"IMPUREMP",8);
    h->version = RECMP_VERSION;
    h->header = sizeof *h;
    h->bytes = bytes;
    h->match = s_match;
    h->stream.Data1 = p->local[0];
    h->origin = p->local[actor];
    h->tick = kind == 3 ? 0 : 1;
    h->sequence = sequence;
    h->kind = kind;
}

static void data_send(Probe* p, RecMpNet* net, unsigned sent)
{
    struct { RecMpHeader h; unsigned char payload[16]; } packet;
    header(p,&packet.h,sent%2,RECMP_RESOURCES,sent/2+1,sizeof packet);
    memset(packet.payload,(int)packet.h.sequence,sizeof packet.payload);
    CHECK(tagpu_recmp_net_send(net,&packet,sizeof packet) == DP_OK);
}

static DWORD WINAPI worker(void* argument)
{
    Probe* p = argument;
    RecMpNet* net = NULL;
    RecMpNetStats stats;
    RecMpSignedIdentity binding[2] = {0};
    GUID bound_stream[2] = {{0}};
    unsigned valid[2] = {0};
    unsigned sent = 0, received = 0, sequences[2] = {0};
    char send_release[MAX_PATH];
    DWORD deadline = GetTickCount() + 45000;
    HRESULT hr = tagpu_recmp_net_open(&p->config,&net);
    printf("PRIVATE OPEN hr=%08lx\n", (unsigned long)hr);
    CHECK(hr == DP_OK && net);
    p->identity = *tagpu_recmp_net_identity(net);
    InterlockedExchange(&p->ready,1);
    printf("PRIVATE READY id=%u\n",p->identity.player);
    while (!atomic(&p->closed)) {
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        Sleep(1);
    }
    CHECK(atomic(&p->remote_count) == 2);
    strcpy(send_release,p->release);
    strcat(send_release,".send");
    while (GetFileAttributesA(send_release) == INVALID_FILE_ATTRIBUTES) {
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        Sleep(1);
    }
    /* Each public key came from its native primary ID. The private binding is
       signed once per origin; ordinary data uses the resulting SDK sender bound. */
    for (sent=0;sent<2;sent++) {
        struct { RecMpHeader h; RecMpSignedIdentity body; } packet;
        header(p,&packet.h,sent,3,0,sizeof packet);
        CHECK(tagpu_recmp_sign(p->key,packet.h.origin,&packet.h.stream,&p->identity,&packet.body));
        CHECK(tagpu_recmp_net_send(net,&packet,sizeof packet) == DP_OK);
    }
    /* Hold Receive while filling the context array. SDK acknowledgments cannot
       release a context until this worker consumes its SENDCOMPLETE. */
    for (sent=0;sent<RECMP_NET_PENDING-2;sent++) data_send(p,net,sent);
    CHECK(tagpu_recmp_net_send(net,"overflow",sizeof "overflow") == DPERR_BUSY);
    CHECK(tagpu_recmp_net_close(net) == DPERR_BUSY);
    tagpu_recmp_net_stats(net,&stats);
    CHECK(stats.pending == RECMP_NET_PENDING && !stats.failed && !stats.completed);
    printf("PENDING 16: seventeenth send and early Close refused\n");
    while (received < 16 || stats.pending || sent < 16) {
        unsigned char buffer[RECMP_PACKET_MAX];
        unsigned bytes = sizeof buffer;
        DWORD from = 0;
        hr = tagpu_recmp_net_receive(net,&from,buffer,&bytes);
        CHECK(hr == DP_OK || hr == DPERR_NOMESSAGES);
        if (hr == DP_OK && from != p->identity.player) {
            RecMpHeader h;
            unsigned actor, i;
            CHECK(bytes >= sizeof h);
            memcpy(&h,buffer,sizeof h);
            CHECK(!memcmp(h.magic,"IMPUREMP",8) && h.version == RECMP_VERSION &&
                  h.header == sizeof h && h.bytes == bytes && !memcmp(&h.match,&s_match,sizeof s_match));
            for (actor=0;actor<2;actor++) if (p->remote[actor].origin == h.origin) break;
            CHECK(actor < 2);
            if (h.kind == 3) {
                CHECK(bytes == sizeof h+sizeof(RecMpSignedIdentity) && !h.sequence && !valid[actor]);
                CHECK(tagpu_recmp_verify(&p->remote[actor].public,h.origin,&h.stream,&p->identity,
                                         from,buffer+sizeof h,bytes-sizeof h,&binding[actor]));
                CHECK(tagpu_recmp_net_authorized(net,&binding[actor].identity,from));
                bound_stream[actor] = h.stream;
                valid[actor] = 1;
                printf("PRIVATE SIGNED BIND actual_from=%lu primary=%u\n",(unsigned long)from,h.origin);
            } else {
                CHECK(h.kind == RECMP_RESOURCES && bytes == sizeof h+16 && valid[actor]);
                CHECK(tagpu_recmp_net_authorized(net,&binding[actor].identity,from));
                CHECK(!memcmp(&bound_stream[actor],&h.stream,sizeof h.stream));
                CHECK(h.sequence == ++sequences[actor]);
                for (i=sizeof h;i<bytes;i++) CHECK(buffer[i] == h.sequence);
                received++;
            }
        }
        tagpu_recmp_net_stats(net,&stats);
        CHECK(!stats.failed && (LONG)(deadline-GetTickCount()) > 0);
        if (sent < 16 && stats.pending < RECMP_NET_PENDING) {
            data_send(p,net,sent++);
            tagpu_recmp_net_stats(net,&stats);
        }
        if (hr == DPERR_NOMESSAGES) WaitForSingleObject(tagpu_recmp_net_event(net),1);
    }
    CHECK(valid[0] && valid[1] && sequences[0] == 8 && sequences[1] == 8);
    CHECK(stats.completed == 18 && stats.completed_bytes ==
          16*(sizeof(RecMpHeader)+16)+2*(sizeof(RecMpHeader)+sizeof(RecMpSignedIdentity)));
    printf("PASS: two primary-native human/AI keys, two signed bindings, 16 private deliveries after primary Close, 18 completions\n");
    while (GetFileAttributesA(p->release) == INVALID_FILE_ATTRIBUTES) {
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        Sleep(10);
    }
    CHECK(tagpu_recmp_net_close(net) == DP_OK);
    return 0;
}

#endif

typedef struct { Probe* p; IDirectPlay4A* primary; const unsigned char* prefix; } ReadPublic;
static BOOL WINAPI public_found(DPID id, DWORD type, LPCDPNAME name, DWORD flags, void* arg)
{
    ReadPublic* read = arg;
    Probe* p = read->p;
    unsigned char data[RECMP_PRIMARY_BYTES+sizeof(RecMpPublic)];
    DWORD bytes = sizeof data;
    DPCAPS caps = {0};
    RecMpPublic public;
    unsigned n = atomic(&p->remote_count), at;
    (void)name; (void)flags;
    CHECK(type == DPPLAYERTYPE_PLAYER);
    if (id == p->local[0] || id == p->local[1]) return TRUE;
    CHECK(IDirectPlayX_GetPlayerData(read->primary,id,data,&bytes,DPGET_REMOTE) == DP_OK);
    CHECK(bytes == sizeof data && !memcmp(data,read->prefix,RECMP_PRIMARY_BYTES));
    CHECK(tagpu_recmp_public_read(data+RECMP_PRIMARY_BYTES,sizeof public,&public));
    caps.dwSize = sizeof caps;
    CHECK(IDirectPlayX_GetPlayerCaps(read->primary,id,&caps,0) == DP_OK &&
          !(caps.dwFlags & DPPLAYERCAPS_LOCAL));
    for (at=0;at<n;at++) if (p->remote[at].origin == id) break;
    if (at == n) {
        CHECK(n < 2);
        p->remote[n].origin = id;
        p->remote[n].public = public;
        InterlockedExchange(&p->remote_count,n+1);
        printf("PRIMARY KEY actual_id=%lu data_bytes=%lu\n",(unsigned long)id,(unsigned long)bytes);
    } else CHECK(!memcmp(&p->remote[at].public,&public,sizeof public));
    return TRUE;
}

int main(int argc, char** argv)
{
    Probe p = {0};
    HMODULE module;
    IDirectPlay4A* primary = NULL;
    IDirectPlayLobby3A* lobby = NULL;
    DPCOMPOUNDADDRESSELEMENT elements[2];
    DPSESSIONDESC2 desc = {0};
    DPNAME name = {0};
    Session session = {{0},0};
    unsigned char address[512], data[4096];
    DWORD bytes = sizeof address, deadline = GetTickCount()+45000, started;
#ifndef RECMP_CHANNEL_TEST
    HANDLE thread;
#endif
    unsigned i, rejected_remote = 0;
    unsigned char userdata[RECMP_PRIMARY_BYTES+sizeof(RecMpPublic)] = {0};
    RecMpPublic public;
    ReadPublic read;
    char advertise_release[MAX_PATH], primary_release[MAX_PATH];
    HRESULT hr;
    CHECK(argc == 3);
    setvbuf(stdout,NULL,_IONBF,0);
    identity_bounds();
    p.config.host = !strcmp(argv[1],"host");
    p.config.match = s_match;
    p.config.provider = DPSPGUID_TCPIP;
    p.release = argv[2];
    CHECK(strlen(p.release)+sizeof ".advertise" <= sizeof advertise_release);
    strcpy(advertise_release,p.release);
    strcat(advertise_release,".advertise");
    strcpy(primary_release,p.release);
    strcat(primary_release,".primary");
    CHECK(SUCCEEDED(CoInitialize(NULL)));
    module = LoadLibraryA("dplayx.dll");
    CHECK(module && create(module,&CLSID_DirectPlay,&IID_IDirectPlay4A,(void**)&primary) == DP_OK);
    CHECK(create(module,&CLSID_DirectPlayLobby,&IID_IDirectPlayLobby3A,(void**)&lobby) == DP_OK);
    elements[0].guidDataType = DPAID_ServiceProvider;
    elements[0].dwDataSize = sizeof(GUID);
    elements[0].lpData = (void*)&DPSPGUID_TCPIP;
    elements[1].guidDataType = DPAID_INet;
    elements[1].dwDataSize = sizeof "127.0.0.1";
    elements[1].lpData = (void*)"127.0.0.1";
    CHECK(IDirectPlayLobby_CreateCompoundAddress(lobby,elements,2,address,&bytes) == DP_OK);
    IDirectPlayLobby_Release(lobby);
    CHECK(IDirectPlayX_InitializeConnection(primary,address,0) == DP_OK);
    desc.dwSize = sizeof desc;
    desc.guidApplication = s_game;
    if (p.config.host) {
        desc.dwMaxPlayers = 4;
        desc.dwFlags = DPSESSION_DIRECTPLAYPROTOCOL | DPSESSION_KEEPALIVE;
        desc.lpszSessionNameA = (char*)"RECMP-NATIVE-BINDING-FIXTURE";
    } else {
        CHECK(IDirectPlayX_EnumSessions(primary,&desc,3000,found,&session,DPENUMSESSIONS_AVAILABLE) == DP_OK);
        CHECK(session.host);
        desc.guidInstance = session.guid;
    }
    CHECK(IDirectPlayX_Open(primary,&desc,p.config.host ? DPOPEN_CREATE : DPOPEN_JOIN) == DP_OK);
    p.key = tagpu_recmp_key();
    CHECK(p.key);
    tagpu_recmp_public(p.key,&public);
    userdata[19] = 0x50;
    memcpy(userdata+RECMP_PRIMARY_BYTES,&public,sizeof public);
    read.p = &p; read.primary = primary; read.prefix = userdata;
    name.dwSize = sizeof name;
    name.lpszLongNameA = (char*)"Fixture player";
    for (i = 0; i < 2; i++) {
        name.lpszShortNameA = i ? (char*)"HostedAI" : (char*)"Human";
        CHECK(IDirectPlayX_CreatePlayer(primary,&p.local[i],&name,NULL,userdata,sizeof userdata,0) == DP_OK);
    }
    if (p.config.host) {
        bytes = sizeof data;
        CHECK(IDirectPlayX_GetSessionDesc(primary,data,&bytes) == DP_OK && bytes <= sizeof data);
        memcpy(&desc,data,sizeof desc);
        desc.dwUser1 = p.local[0];
        CHECK(IDirectPlayX_SetSessionDesc(primary,&desc,0) == DP_OK);
        session.host = p.local[0];
    }
    bytes = sizeof p.config.address;
    CHECK(IDirectPlayX_GetPlayerAddress(primary,session.host,p.config.address,&bytes) == DP_OK);
    CHECK(bytes && bytes <= sizeof p.config.address);
    p.config.address_bytes = bytes;
    printf("PRIMARY READY human=%lu AI=%lu address_bytes=%lu\n",
        (unsigned long)p.local[0],(unsigned long)p.local[1],(unsigned long)bytes);
#ifndef RECMP_CHANNEL_TEST
    thread = CreateThread(NULL,0,worker,&p,0,NULL);
    CHECK(thread);
#endif
    while (atomic(&p.remote_count) < 2) {
        DPID from = 0, to = 0;
        bytes = sizeof data;
        hr = IDirectPlayX_Receive(primary,&from,&to,DPRECEIVE_ALL,data,&bytes);
        CHECK(hr == DP_OK || hr == DPERR_NOMESSAGES);
        if (GetFileAttributesA(advertise_release) != INVALID_FILE_ATTRIBUTES) {
            CHECK(IDirectPlayX_EnumPlayers(primary,NULL,public_found,&read,DPENUMPLAYERS_REMOTE) == DP_OK);
            if (atomic(&p.remote_count) && !rejected_remote) {
                hr = IDirectPlayX_SetPlayerData(primary,p.remote[0].origin,userdata,sizeof userdata,DPSET_REMOTE);
                CHECK(hr == DPERR_ACCESSDENIED);
                printf("PRIMARY refused remote-owned data change hr=%08lx\n",(unsigned long)hr);
                hr = IDirectPlayX_Send(primary,p.remote[0].origin,p.local[0],0,(void*)"forged",sizeof "forged");
                CHECK(hr == DPERR_ACCESSDENIED || hr == DPERR_INVALIDPLAYER);
                rejected_remote = 1;
                printf("PRIMARY refused remote-source Send hr=%08lx\n",(unsigned long)hr);
            }
        }
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        if (hr == DPERR_NOMESSAGES) Sleep(1);
    }
    CHECK(rejected_remote);
    /* The runner observes both ready publications before releasing either
       primary session. A guaranteed send acknowledges transport delivery, not
       the other application's consumption of its identity message. */
    printf("PRIMARY KEYS READY\n");
    while (GetFileAttributesA(primary_release) == INVALID_FILE_ATTRIBUTES) {
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        Sleep(1);
    }
    started = GetTickCount();
    CHECK(IDirectPlayX_Close(primary) == DP_OK);
    started = GetTickCount()-started;
    IDirectPlayX_Release(primary);
    InterlockedExchange(&p.closed,1);
    printf("PRIMARY CLOSED elapsed_ms=%lu\n",(unsigned long)started);
#ifdef RECMP_CHANNEL_TEST
    channel_run(&p);
#else
    CHECK(WaitForSingleObject(thread,50000) == WAIT_OBJECT_0);
    CloseHandle(thread);
#endif
    FreeLibrary(module);
    CoUninitialize();
    return 0;
}
