/* Native TCP/IP fixture for the recorder's isolated transport. Run a host and
   joiner on the same private DirectPlay discovery port, passing a release-file
   path to each. It stands in for TA's primary session: identities travel from
   real human/AI native senders, before both primary interfaces close. */
#define INITGUID
#define COBJMACROS
#define _WIN32_WINNT 0x0400
#include "tagpu_recmp_net.h"
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
    struct { DWORD origin; RecMpIdentity identity; } remote[2];
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

static DWORD WINAPI worker(void* argument)
{
    Probe* p = argument;
    RecMpNet* net = NULL;
    RecMpNetStats stats;
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
    /* Hold Receive while filling the context array. The SDK can acknowledge
       packets meanwhile; their contexts remain pending until we consume completions. */
    for (sent = 0; sent < RECMP_NET_PENDING; sent++) {
        struct { RecMpHeader h; unsigned char payload[16]; } packet = {0};
        memcpy(packet.h.magic,"IMPUREMP",8);
        packet.h.version = 1;
        packet.h.header = sizeof packet.h;
        packet.h.bytes = sizeof packet;
        packet.h.match = s_match;
        packet.h.stream.Data1 = p->local[0];
        packet.h.origin = p->local[sent%2];
        packet.h.tick = 1;
        packet.h.sequence = sent/2+1;
        packet.h.kind = RECMP_RESOURCES;
        memset(packet.payload,(int)packet.h.sequence,sizeof packet.payload);
        CHECK(tagpu_recmp_net_send(net,&packet,sizeof packet) == DP_OK);
    }
    CHECK(tagpu_recmp_net_send(net,"overflow",sizeof "overflow") == DPERR_BUSY);
    CHECK(tagpu_recmp_net_close(net) == DPERR_BUSY);
    tagpu_recmp_net_stats(net,&stats);
    CHECK(stats.pending == RECMP_NET_PENDING && !stats.failed && !stats.completed);
    printf("PENDING 16: seventeenth send and early Close refused\n");
    while (received < 16 || stats.pending) {
        unsigned char buffer[RECMP_PACKET_MAX];
        unsigned bytes = sizeof buffer;
        DWORD from = 0;
        hr = tagpu_recmp_net_receive(net,&from,buffer,&bytes);
        CHECK(hr == DP_OK || hr == DPERR_NOMESSAGES);
        if (hr == DP_OK && from != p->identity.player) {
            RecMpHeader header;
            RecMpIdentity wrong;
            unsigned actor, i;
            CHECK(bytes == sizeof header+16);
            memcpy(&header,buffer,sizeof header);
            for (actor = 0; actor < 2; actor++) if (p->remote[actor].origin == header.origin) break;
            CHECK(actor < 2 && tagpu_recmp_net_authorized(net,&p->remote[actor].identity,from));
            CHECK(!tagpu_recmp_net_authorized(net,&p->remote[actor].identity,p->identity.player));
            wrong = p->remote[actor].identity;
            wrong.player ^= 1;
            CHECK(!tagpu_recmp_net_authorized(net,&wrong,from));
            wrong = p->remote[actor].identity;
            wrong.session.Data1 ^= 1;
            CHECK(!tagpu_recmp_net_authorized(net,&wrong,from));
            wrong = p->remote[actor].identity;
            wrong.match.Data1 ^= 1;
            CHECK(!tagpu_recmp_net_authorized(net,&wrong,from));
            CHECK(header.sequence == ++sequences[actor]);
            for (i = sizeof header; i < bytes; i++) CHECK(buffer[i] == header.sequence);
            received++;
        }
        tagpu_recmp_net_stats(net,&stats);
        CHECK(!stats.failed && (LONG)(deadline-GetTickCount()) > 0);
        if (hr == DPERR_NOMESSAGES) WaitForSingleObject(tagpu_recmp_net_event(net),1);
    }
    CHECK(stats.completed == 16 && stats.completed_bytes == 16*(sizeof(RecMpHeader)+16));
    CHECK(sequences[0] == 8 && sequences[1] == 8);
    printf("PASS: two primary-native human/AI bindings, 16 private deliveries after primary Close, 16 completions\n");
    while (GetFileAttributesA(p->release) == INVALID_FILE_ATTRIBUTES) {
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        Sleep(10);
    }
    CHECK(tagpu_recmp_net_close(net) == DP_OK);
    return 0;
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
    HANDLE thread;
    unsigned i, advertised = 0, rejected_remote = 0;
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
    name.dwSize = sizeof name;
    name.lpszLongNameA = (char*)"Fixture player";
    for (i = 0; i < 2; i++) {
        name.lpszShortNameA = i ? (char*)"HostedAI" : (char*)"Human";
        CHECK(IDirectPlayX_CreatePlayer(primary,&p.local[i],&name,NULL,NULL,0,0) == DP_OK);
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
    thread = CreateThread(NULL,0,worker,&p,0,NULL);
    CHECK(thread);
    while (atomic(&p.remote_count) < 2) {
        DPID from = 0, to = 0;
        bytes = sizeof data;
        hr = IDirectPlayX_Receive(primary,&from,&to,DPRECEIVE_ALL,data,&bytes);
        CHECK(hr == DP_OK || hr == DPERR_NOMESSAGES);
        if (hr == DP_OK && from && from != p.local[0] && from != p.local[1]) {
            RecMpIdentity identity;
            DPCAPS caps = {0};
            unsigned n = atomic(&p.remote_count), at;
            CHECK(bytes == 58+sizeof identity && data[0] == 0x28);
            CHECK(tagpu_recmp_identity_read(data+58,sizeof identity,&s_match,&identity));
            caps.dwSize = sizeof caps;
            CHECK(IDirectPlayX_GetPlayerCaps(primary,from,&caps,0) == DP_OK);
            CHECK(!(caps.dwFlags & DPPLAYERCAPS_LOCAL));
            for (at = 0; at < n; at++) if (p.remote[at].origin == from) break;
            if (at == n) {
                CHECK(n < 2);
                p.remote[n].origin = from;
                p.remote[n].identity = identity;
                InterlockedExchange(&p.remote_count,n+1);
                printf("PRIMARY BIND actual_from=%lu private_id=%u\n",(unsigned long)from,identity.player);
            }
            if (!rejected_remote) {
                hr = IDirectPlayX_Send(primary,from,p.local[0],0,(void*)"forged",sizeof "forged");
                CHECK(hr == DPERR_ACCESSDENIED || hr == DPERR_INVALIDPLAYER);
                rejected_remote = 1;
                printf("PRIMARY refused remote-source Send hr=%08lx\n",(unsigned long)hr);
            }
        }
        if (!advertised && atomic(&p.ready) &&
            GetFileAttributesA(advertise_release) != INVALID_FILE_ATTRIBUTES) {
            unsigned char message[58+sizeof(RecMpIdentity)] = {0};
            message[0] = 0x28;
            memcpy(message+58,&p.identity,sizeof p.identity);
            for (i = 0; i < 2; i++)
                CHECK(IDirectPlayX_Send(primary,p.local[i],DPID_ALLPLAYERS,DPSEND_GUARANTEED,message,sizeof message) == DP_OK);
            advertised = 1;
        }
        CHECK((LONG)(deadline-GetTickCount()) > 0);
        if (hr == DPERR_NOMESSAGES) Sleep(1);
    }
    CHECK(advertised && rejected_remote);
    /* The runner observes both ready publications before releasing either
       primary session. A guaranteed send acknowledges transport delivery, not
       the other application's consumption of its identity message. */
    printf("PRIMARY BINDINGS READY\n");
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
    CHECK(WaitForSingleObject(thread,50000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    FreeLibrary(module);
    CoUninitialize();
    return 0;
}
