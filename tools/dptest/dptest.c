/* dptest.c - minimal DirectPlay TCP/IP host/join probe.
 * Mirrors what a 1997-era game (e.g. Total Annihilation) asks of dplayx+dpwsockx:
 * pick the TCP/IP service provider, host a session, enumerate it, join it,
 * create players and push one game message across.
 *
 *   dptest.exe host [addr]   - create a session and receive
 *   dptest.exe enum [addr]   - enumerate sessions only
 *   dptest.exe join [addr]   - enumerate, join the first hit, send a message
 */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define _WIN32_WINNT 0x0400
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <dplay.h>
#include <dplobby.h>

/* An application GUID of our own, standing in for the game's. */
DEFINE_GUID(APPGUID, 0xdeadbeef,0x1234,0x5678,0x9a,0xbc,0xde,0xf0,0x11,0x22,0x33,0x44);

static const char *hr_name(HRESULT hr)
{
    switch ((DWORD)hr) {
    case DP_OK:                    return "DP_OK";
    case DPERR_NOSESSIONS:         return "DPERR_NOSESSIONS";
    case DPERR_USERCANCEL:         return "DPERR_USERCANCEL";
    case DPERR_UNAVAILABLE:        return "DPERR_UNAVAILABLE";
    case DPERR_UNSUPPORTED:        return "DPERR_UNSUPPORTED";
    case DPERR_INVALIDPARAMS:      return "DPERR_INVALIDPARAMS";
    case DPERR_INVALIDOBJECT:      return "DPERR_INVALIDOBJECT";
    case DPERR_UNINITIALIZED:      return "DPERR_UNINITIALIZED";
    case DPERR_NOCONNECTION:       return "DPERR_NOCONNECTION";
    case DPERR_TIMEOUT:            return "DPERR_TIMEOUT";
    case DPERR_BUSY:               return "DPERR_BUSY";
    case DPERR_CONNECTING:         return "DPERR_CONNECTING";
    case DPERR_NOMESSAGES:         return "DPERR_NOMESSAGES";
    case DPERR_GENERIC:            return "DPERR_GENERIC";
    case DPERR_EXCEPTION:          return "DPERR_EXCEPTION";
    case DPERR_ACCESSDENIED:       return "DPERR_ACCESSDENIED";
    case DPERR_BUFFERTOOSMALL:     return "DPERR_BUFFERTOOSMALL";
    case DPERR_ALREADYINITIALIZED: return "DPERR_ALREADYINITIALIZED";
    case E_NOINTERFACE:            return "E_NOINTERFACE";
    case E_OUTOFMEMORY:            return "E_OUTOFMEMORY";
    }
    return "?";
}
#define CHECK(label, hr) \
    printf("[%s] %-28s hr=0x%08lx %s\n", g_role, (label), (unsigned long)(hr), hr_name(hr)); fflush(stdout)

static const char *g_role = "?";
static GUID  g_found_session;
static int   g_found = 0;

static BOOL WINAPI enum_cb(LPCDPSESSIONDESC2 sd, LPDWORD timeout, DWORD flags, LPVOID ctx)
{
    (void)timeout; (void)ctx;
    if (flags & DPESC_TIMEDOUT) {
        printf("[%s]   ...enum timed out\n", g_role); fflush(stdout);
        return FALSE;
    }
    printf("[%s]   SESSION \"%s\" players=%lu/%lu flags=0x%lx\n", g_role,
           sd->lpszSessionNameA ? sd->lpszSessionNameA : "(null)",
           (unsigned long)sd->dwCurrentPlayers, (unsigned long)sd->dwMaxPlayers,
           (unsigned long)sd->dwFlags);
    fflush(stdout);
    if (!g_found) { g_found_session = sd->guidInstance; g_found = 1; }
    return TRUE;
}

/* Build the DPADDRESS the TCP/IP SP wants: {provider GUID, inet address string}. */
static void *build_address(const char *inet, DWORD *size)
{
    IDirectPlayLobby3A *lobby = NULL;
    DPCOMPOUNDADDRESSELEMENT elems[2];
    void *addr = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_DirectPlayLobby, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IDirectPlayLobby3A, (void **)&lobby);
    CHECK("CoCreateInstance(Lobby3A)", hr);
    if (FAILED(hr)) return NULL;

    elems[0].guidDataType = DPAID_ServiceProvider;
    elems[0].dwDataSize   = sizeof(GUID);
    elems[0].lpData       = (void *)&DPSPGUID_TCPIP;
    elems[1].guidDataType = DPAID_INet;
    elems[1].dwDataSize   = (DWORD)strlen(inet) + 1;
    elems[1].lpData       = (void *)inet;

    *size = 0;
    hr = IDirectPlayLobby_CreateCompoundAddress(lobby, elems, 2, NULL, size);
    if (hr == DPERR_BUFFERTOOSMALL) {
        addr = HeapAlloc(GetProcessHeap(), 0, *size);
        hr = IDirectPlayLobby_CreateCompoundAddress(lobby, elems, 2, addr, size);
    }
    CHECK("CreateCompoundAddress", hr);
    IDirectPlayLobby_Release(lobby);
    return SUCCEEDED(hr) ? addr : NULL;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "enum";
    const char *inet = argc > 2 ? argv[2] : "127.0.0.1";
    IDirectPlay4A *dp = NULL;
    DPSESSIONDESC2 sd;
    DPNAME name;
    DPID pid = 0;
    void *addr;
    DWORD addrsize = 0;
    HRESULT hr;
    int i;

    g_role = mode;
    printf("[%s] === dptest, provider=TCP/IP, inet=%s ===\n", g_role, inet); fflush(stdout);

    hr = CoInitialize(NULL);
    CHECK("CoInitialize", hr);

    hr = CoCreateInstance(&CLSID_DirectPlay, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IDirectPlay4A, (void **)&dp);
    CHECK("CoCreateInstance(DirectPlay4A)", hr);
    if (FAILED(hr)) return 1;

    addr = build_address(inet, &addrsize);
    if (!addr) return 1;

    hr = IDirectPlayX_InitializeConnection(dp, addr, 0);
    CHECK("InitializeConnection", hr);
    if (FAILED(hr)) return 1;

    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidApplication = APPGUID;

    if (!strcmp(mode, "host")) {
        sd.dwFlags = DPSESSION_KEEPALIVE | DPSESSION_MIGRATEHOST;
        sd.dwMaxPlayers = 4;
        sd.lpszSessionNameA = (char *)"DPTEST-SESSION";
        hr = IDirectPlayX_Open(dp, &sd, DPOPEN_CREATE);
        CHECK("Open(DPOPEN_CREATE)", hr);
        if (FAILED(hr)) return 1;

        ZeroMemory(&name, sizeof(name));
        name.dwSize = sizeof(name);
        name.lpszShortNameA = (char *)"HostPlayer";
        hr = IDirectPlayX_CreatePlayer(dp, &pid, &name, NULL, NULL, 0, 0);
        CHECK("CreatePlayer", hr);
        printf("[%s] host player id = 0x%lx -- receiving for 25s\n", g_role, (unsigned long)pid);
        fflush(stdout);

        for (i = 0; i < 250; i++) {
            DPID from = 0, to = 0;
            char buf[512];
            DWORD sz = sizeof(buf);
            hr = IDirectPlayX_Receive(dp, &from, &to, DPRECEIVE_ALL, buf, &sz);
            if (hr == DP_OK) {
                if (from == DPID_SYSMSG) {
                    DPMSG_GENERIC *g = (DPMSG_GENERIC *)buf;
                    printf("[%s]   SYSMSG type=%lu\n", g_role, (unsigned long)g->dwType);
                } else {
                    printf("[%s]   *** GAME MSG from=0x%lx to=0x%lx len=%lu: \"%.*s\"\n",
                           g_role, (unsigned long)from, (unsigned long)to,
                           (unsigned long)sz, (int)sz, buf);
                }
                fflush(stdout);
            }
            Sleep(100);
        }
        IDirectPlayX_Close(dp);
        printf("[%s] done\n", g_role); fflush(stdout);
        return 0;
    }

    /* enum + join share the enumeration step */
    printf("[%s] EnumSessions...\n", g_role); fflush(stdout);
    hr = IDirectPlayX_EnumSessions(dp, &sd, 5000, enum_cb, NULL, DPENUMSESSIONS_AVAILABLE);
    CHECK("EnumSessions", hr);
    printf("[%s] sessions found = %d\n", g_role, g_found); fflush(stdout);

    if (!strcmp(mode, "enum") || !g_found) return g_found ? 0 : 2;

    ZeroMemory(&sd, sizeof(sd));
    sd.dwSize = sizeof(sd);
    sd.guidInstance = g_found_session;
    sd.guidApplication = APPGUID;
    hr = IDirectPlayX_Open(dp, &sd, DPOPEN_JOIN);
    CHECK("Open(DPOPEN_JOIN)", hr);
    if (FAILED(hr)) return 1;

    ZeroMemory(&name, sizeof(name));
    name.dwSize = sizeof(name);
    name.lpszShortNameA = (char *)"JoinPlayer";
    hr = IDirectPlayX_CreatePlayer(dp, &pid, &name, NULL, NULL, 0, 0);
    CHECK("CreatePlayer", hr);
    if (FAILED(hr)) return 1;
    printf("[%s] join player id = 0x%lx\n", g_role, (unsigned long)pid); fflush(stdout);

    for (i = 0; i < 3; i++) {
        char msg[64];
        sprintf(msg, "hello-from-join-%d", i);
        hr = IDirectPlayX_SendEx(dp, pid, DPID_ALLPLAYERS, DPSEND_GUARANTEED,
                                 msg, (DWORD)strlen(msg) + 1, 0, 0, NULL, NULL);
        CHECK("SendEx", hr);
        Sleep(500);
    }
    Sleep(1000);
    IDirectPlayX_Close(dp);
    printf("[%s] done\n", g_role); fflush(stdout);
    return 0;
}
