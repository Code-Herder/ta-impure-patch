#define INITGUID
#define COBJMACROS
#include "tagpu_recmp_net.h"
#include <dplobby.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

static const GUID s_app = { 0x8d160302,0x4589,0x40ba,{0xa2,0x67,0x23,0x5a,0x1d,0x70,0x36,0x01} };

struct RecMpNet {
    HMODULE module;
    IDirectPlay4A* dp;
    HANDLE event;
    RecMpIdentity identity;
    struct { unsigned used, bytes; DWORD id; } send[RECMP_NET_PENDING];
    RecMpNetStats stats;
    unsigned com, opened;
};

static int guid_present(const GUID* guid)
{
    static const GUID zero = {0};
    return memcmp(guid, &zero, sizeof zero) != 0;
}

int tagpu_recmp_identity_read(const void* bytes, unsigned size, const GUID* match,
                              RecMpIdentity* out)
{
    RecMpIdentity identity;
    if (!bytes || !match || !out || size != sizeof identity) return 0;
    memcpy(&identity, bytes, sizeof identity);
    if (identity.version != RECMP_VERSION || identity.bytes != sizeof identity ||
        !identity.player || !guid_present(match) || !guid_present(&identity.session) ||
        memcmp(&identity.match, match, sizeof *match)) return 0;
    *out = identity;
    return 1;
}

static HRESULT object(HMODULE module, const CLSID* clsid, const IID* iid, void** out)
{
    typedef HRESULT (WINAPI *GetClass)(const CLSID*, const IID*, void**);
    GetClass get_class = (GetClass)GetProcAddress(module, "DllGetClassObject");
    IClassFactory* factory = NULL;
    HRESULT hr;
    if (!get_class) return E_NOINTERFACE;
    hr = get_class(clsid, &IID_IClassFactory, (void**)&factory);
    if (FAILED(hr)) return hr;
    hr = IClassFactory_CreateInstance(factory, NULL, iid, out);
    IClassFactory_Release(factory);
    return hr;
}

typedef struct { unsigned bytes; unsigned char value[256]; } InetAddress;
typedef struct { unsigned count, invalid; InetAddress address[32]; } Inet;

static BOOL CALLBACK inet_address(REFGUID type, DWORD bytes, const void* data, void* arg)
{
    Inet* inet = (Inet*)arg;
    unsigned i, at;
    /* GetPlayerAddress supplies both ANSI and Unicode forms, including several
       addresses for a local player. Retain each distinct ANSI address, not a
       guessed primary adapter; discovery tries the bounded list. */
    if (memcmp(type, &DPAID_INet, sizeof *type)) return TRUE;
    if (!bytes || bytes > sizeof inet->address[0].value || !data) goto invalid;
    for (i = 0; i < bytes; i++) if (!((const unsigned char*)data)[i]) break;
    if (!i || i == bytes) goto invalid;
    bytes = i + 1;
    for (at = 0; at < inet->count; at++)
        if (inet->address[at].bytes == bytes && !memcmp(inet->address[at].value, data, bytes)) return TRUE;
    if (inet->count == sizeof inet->address / sizeof inet->address[0]) goto invalid;
    inet->address[inet->count].bytes = bytes;
    memcpy(inet->address[inet->count++].value, data, bytes);
    return TRUE;
invalid:
    inet->invalid = 1;
    return FALSE;
}

static HRESULT addresses(HMODULE module, const RecMpNetConfig* config, Inet* inet)
{
    IDirectPlayLobby3A* lobby = NULL;
    HRESULT hr = object(module, &CLSID_DirectPlayLobby, &IID_IDirectPlayLobby3A, (void**)&lobby);
    if (FAILED(hr)) return hr;
    hr = IDirectPlayLobby_EnumAddress(lobby, inet_address, config->address, config->address_bytes, inet);
    IDirectPlayLobby_Release(lobby);
    if (hr == DP_OK && (inet->invalid || !inet->count)) hr = DPERR_INVALIDPARAMS;
    return hr;
}

static HRESULT connection(RecMpNet* net, const GUID* provider, const InetAddress* inet)
{
    IDirectPlayLobby3A* lobby = NULL;
    DPCOMPOUNDADDRESSELEMENT element[2];
    unsigned char address[RECMP_NET_ADDRESS];
    DWORD bytes = sizeof address;
    HRESULT hr = object(net->module, &CLSID_DirectPlayLobby, &IID_IDirectPlayLobby3A, (void**)&lobby);
    if (FAILED(hr)) return hr;
    element[0].guidDataType = DPAID_ServiceProvider;
    element[0].dwDataSize = sizeof *provider;
    element[0].lpData = (void*)provider;
    element[1].guidDataType = DPAID_INet;
    element[1].dwDataSize = inet->bytes;
    element[1].lpData = (void*)inet->value;
    /* The primary player's dynamic port is not the session-discovery address.
       Preserve the provider and IP, and let the provider choose this session's port. */
    hr = IDirectPlayLobby_CreateCompoundAddress(lobby, element, 2, address, &bytes);
    if (hr == DP_OK && bytes <= sizeof address)
        hr = IDirectPlayX_InitializeConnection(net->dp, address, 0);
    else if (hr == DP_OK) hr = DPERR_INVALIDPARAMS;
    IDirectPlayLobby_Release(lobby);
    return hr;
}

typedef struct { char name[48]; GUID session; unsigned found; } Search;

static BOOL WINAPI session_found(LPCDPSESSIONDESC2 desc, LPDWORD timeout, DWORD flags, void* arg)
{
    Search* search = (Search*)arg;
    unsigned n;
    (void)timeout;
    if (flags & DPESC_TIMEDOUT) return FALSE;
    if (desc->dwSize != sizeof *desc || !desc->lpszSessionNameA ||
        memcmp(&desc->guidApplication, &s_app, sizeof s_app)) return TRUE;
    /* The session name belongs to the SDK for this callback. Compare only the
       bounded name we created, including its NUL; no unbounded string scan. */
    for (n = 0; n < sizeof search->name; n++) {
        if (desc->lpszSessionNameA[n] != search->name[n]) return TRUE;
        if (!search->name[n]) break;
    }
    if (n == sizeof search->name || !guid_present(&desc->guidInstance)) return TRUE;
    if (search->found && memcmp(&search->session, &desc->guidInstance, sizeof search->session)) {
        search->found = 2;
        return FALSE;
    }
    search->session = desc->guidInstance;
    search->found = 1;
    return TRUE;
}

static void destroy(RecMpNet* net)
{
    if (net->dp) {
        if (net->opened) IDirectPlayX_Close(net->dp);
        IDirectPlayX_Release(net->dp);
    }
    if (net->event) CloseHandle(net->event);
    if (net->module) FreeLibrary(net->module);
    if (net->com) CoUninitialize();
    VirtualFree(net, 0, MEM_RELEASE);
}

HRESULT tagpu_recmp_net_open(const RecMpNetConfig* config, RecMpNet** out)
{
    RecMpNet* net;
    Search search = {0};
    Inet inet = {0};
    DPSESSIONDESC2 desc;
    DPNAME name = {0};
    DPCAPS caps = {0};
    unsigned char session[4096];
    DWORD bytes = sizeof session;
    DWORD player = 0;
    unsigned attempt;
    HRESULT hr;
    if (!out) return E_INVALIDARG;
    *out = NULL;
    if (!config || !guid_present(&config->match) || config->host > 1 ||
        !config->address_bytes || config->address_bytes > sizeof config->address) return E_INVALIDARG;
    /* This adapter currently has a native TCP/IP address contract. Unsupported
       providers refuse recording; they must not be reinterpreted as an IP address. */
    if (memcmp(&config->provider, &DPSPGUID_TCPIP, sizeof config->provider)) return DPERR_UNSUPPORTED;
    net = (RecMpNet*)VirtualAlloc(NULL, sizeof *net, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!net) return E_OUTOFMEMORY;
    hr = CoInitialize(NULL);
    if (FAILED(hr)) goto fail;
    net->com = 1;
    net->module = LoadLibraryA("dplayx.dll");
    if (!net->module) { hr = HRESULT_FROM_WIN32(GetLastError()); goto fail; }
    hr = object(net->module, &CLSID_DirectPlay, &IID_IDirectPlay4A, (void**)&net->dp);
    if (hr != DP_OK) goto fail;
    hr = addresses(net->module, config, &inet);
    if (hr != DP_OK) goto fail;
    snprintf(search.name, sizeof search.name, "IMPUREMP-%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
        (unsigned long)config->match.Data1, config->match.Data2, config->match.Data3,
        config->match.Data4[0], config->match.Data4[1], config->match.Data4[2], config->match.Data4[3],
        config->match.Data4[4], config->match.Data4[5], config->match.Data4[6], config->match.Data4[7]);
    memset(&desc, 0, sizeof desc);
    desc.dwSize = sizeof desc;
    desc.guidApplication = s_app;
    if (config->host) {
        hr = connection(net, &config->provider, &inet.address[0]);
        if (hr != DP_OK) goto fail;
        desc.dwMaxPlayers = 10;
        desc.dwFlags = DPSESSION_DIRECTPLAYPROTOCOL | DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
        desc.lpszSessionNameA = search.name;
    } else {
        for (attempt = 0; attempt < inet.count && !search.found; attempt++) {
            if (attempt) {
                IDirectPlayX_Release(net->dp);
                net->dp = NULL;
                hr = object(net->module, &CLSID_DirectPlay, &IID_IDirectPlay4A, (void**)&net->dp);
                if (hr != DP_OK) goto fail;
            }
            hr = connection(net, &config->provider, &inet.address[attempt]);
            if (hr != DP_OK) goto fail;
            hr = IDirectPlayX_EnumSessions(net->dp, &desc, 1500, session_found, &search,
                                           DPENUMSESSIONS_AVAILABLE);
            if (hr != DP_OK && hr != DPERR_NOSESSIONS) goto fail;
        }
        if (search.found != 1) { hr = DPERR_NOSESSIONS; goto fail; }
        desc.guidInstance = search.session;
    }
    hr = IDirectPlayX_Open(net->dp, &desc, config->host ? DPOPEN_CREATE : DPOPEN_JOIN);
    if (hr != DP_OK) goto fail;
    net->opened = 1;
    hr = IDirectPlayX_GetSessionDesc(net->dp, session, &bytes);
    if (hr != DP_OK || bytes > sizeof session || bytes < sizeof desc) {
        if (hr == DP_OK) hr = E_INVALIDARG;
        goto fail;
    }
    memcpy(&desc, session, sizeof desc);
    if (desc.dwSize != sizeof desc || !guid_present(&desc.guidInstance) ||
        memcmp(&desc.guidApplication, &s_app, sizeof s_app) ||
        !(desc.dwFlags & DPSESSION_DIRECTPLAYPROTOCOL) ||
        (desc.dwFlags & (DPSESSION_NOMESSAGEID | DPSESSION_NOPRESERVEORDER))) {
        hr = DPERR_UNSUPPORTED;
        goto fail;
    }
    caps.dwSize = sizeof caps;
    hr = IDirectPlayX_GetCaps(net->dp, &caps, DPGETCAPS_GUARANTEED);
    if (hr != DP_OK) goto fail;
    if ((caps.dwFlags & (DPCAPS_ASYNCSUPPORTED | DPCAPS_ASYNCCANCELSUPPORTED | DPCAPS_SENDTIMEOUTSUPPORTED)) !=
        (DPCAPS_ASYNCSUPPORTED | DPCAPS_ASYNCCANCELSUPPORTED | DPCAPS_SENDTIMEOUTSUPPORTED)) {
        hr = DPERR_UNSUPPORTED;
        goto fail;
    }
    net->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!net->event) { hr = HRESULT_FROM_WIN32(GetLastError()); goto fail; }
    name.dwSize = sizeof name;
    name.lpszShortNameA = (char*)"Impure";
    name.lpszLongNameA = (char*)"Impure recorder";
    hr = IDirectPlayX_CreatePlayer(net->dp, &player, &name, net->event, NULL, 0, 0);
    if (hr != DP_OK) goto fail;
    net->identity.player = player;
    net->identity.version = RECMP_VERSION;
    net->identity.bytes = sizeof net->identity;
    net->identity.match = config->match;
    net->identity.session = desc.guidInstance;
    *out = net;
    return DP_OK;
fail:
    destroy(net);
    return hr;
}

const RecMpIdentity* tagpu_recmp_net_identity(const RecMpNet* net) { return &net->identity; }
HANDLE tagpu_recmp_net_event(const RecMpNet* net) { return net->event; }

int tagpu_recmp_net_authorized(const RecMpNet* net, const RecMpIdentity* binding, DWORD from)
{
    RecMpIdentity checked;
    return net && binding && from && from != net->identity.player &&
        tagpu_recmp_identity_read(binding, sizeof *binding, &net->identity.match, &checked) &&
        checked.player == from && !memcmp(&checked.session, &net->identity.session, sizeof checked.session);
}

HRESULT tagpu_recmp_net_send(RecMpNet* net, const void* packet, unsigned bytes)
{
    unsigned slot;
    HRESULT hr;
    if (!net || !packet || !bytes || bytes > RECMP_PACKET_MAX) return E_INVALIDARG;
    if (net->stats.failed) return net->stats.failed;
    for (slot = 0; slot < RECMP_NET_PENDING; slot++) if (!net->send[slot].used) break;
    if (slot == RECMP_NET_PENDING) return DPERR_BUSY;
    hr = IDirectPlayX_SendEx(net->dp, net->identity.player, DPID_ALLPLAYERS,
                            DPSEND_GUARANTEED | DPSEND_ASYNC, (void*)packet, bytes,
                            0, 10000, &net->send[slot], &net->send[slot].id);
    if (hr != DPERR_PENDING) {
        net->stats.failed = hr == DP_OK ? E_UNEXPECTED : hr;
        return net->stats.failed;
    }
    /* The SDK copies an accepted asynchronous send. The caller may reuse its
       packet after this return; this fixed context remains until SENDCOMPLETE. */
    net->send[slot].used = 1;
    net->send[slot].bytes = bytes;
    net->stats.pending++;
    return DP_OK;
}

HRESULT tagpu_recmp_net_receive(RecMpNet* net, DWORD* from, void* packet, unsigned* bytes)
{
    DWORD source = 0, target = 0, size;
    HRESULT hr;
    if (!net || !from || !packet || !bytes || *bytes > RECMP_PACKET_MAX) return E_INVALIDARG;
    if (net->stats.failed) return net->stats.failed;
    size = *bytes;
    hr = IDirectPlayX_Receive(net->dp, &source, &target, DPRECEIVE_ALL, packet, &size);
    if (hr != DP_OK) {
        if (hr != DPERR_NOMESSAGES) net->stats.failed = hr;
        return hr;
    }
    if (size > *bytes) { net->stats.failed = E_INVALIDARG; return net->stats.failed; }
    if (source) { *from = source; *bytes = size; return DP_OK; }
    if (size >= sizeof(DPMSG_GENERIC)) {
        DPMSG_GENERIC generic;
        memcpy(&generic, packet, sizeof generic);
        if (generic.dwType == DPSYS_SENDCOMPLETE) {
            DPMSG_SENDCOMPLETE complete;
            uintptr_t offset;
            unsigned slot;
            if (size != sizeof complete) goto invalid;
            memcpy(&complete, packet, sizeof complete);
            offset = (uintptr_t)complete.lpvContext - (uintptr_t)&net->send[0];
            if (offset % sizeof net->send[0] || offset / sizeof net->send[0] >= RECMP_NET_PENDING) goto invalid;
            slot = (unsigned)(offset / sizeof net->send[0]);
            if (!net->send[slot].used || complete.dwMsgID != net->send[slot].id ||
                complete.idFrom != net->identity.player) goto invalid;
            net->send[slot].used = 0;
            net->stats.pending--;
            if (complete.hr != DP_OK) { net->stats.failed = complete.hr; return complete.hr; }
            net->stats.completed++;
            net->stats.completed_bytes += net->send[slot].bytes;
        } else if (generic.dwType == DPSYS_SESSIONLOST) {
            net->stats.failed = DPERR_CONNECTIONLOST;
            return net->stats.failed;
        }
    } else goto invalid;
    *bytes = 0;
    return DPERR_NOMESSAGES;
invalid:
    net->stats.failed = E_INVALIDARG;
    return net->stats.failed;
}

void tagpu_recmp_net_stats(const RecMpNet* net, RecMpNetStats* out) { *out = net->stats; }

HRESULT tagpu_recmp_net_close(RecMpNet* net)
{
    HRESULT hr;
    if (!net) return E_INVALIDARG;
    if (net->stats.pending) return DPERR_BUSY;
    hr = IDirectPlayX_Close(net->dp);
    net->opened = 0;
    destroy(net);
    return hr;
}

void tagpu_recmp_net_abort(RecMpNet* net)
{
    unsigned slot;
    if (!net) return;
    for (slot = 0; slot < RECMP_NET_PENDING; slot++)
        if (net->send[slot].used) IDirectPlayX_CancelMessage(net->dp, net->send[slot].id, 0);
    /* No application callback is installed. Only this owner can Receive a context;
       Close/Release ends that path before context/event/module storage is released. */
    destroy(net);
}
