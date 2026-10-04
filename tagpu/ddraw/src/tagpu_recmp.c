#include "tagpu_recmp.h"
#include <string.h>

#define MP_QUEUE_SLOTS 256u
#define MP_RECEIVE_BUDGET 32u
static const unsigned char s_magic[8] = { 'I','M','P','U','R','E','M','P' };

typedef struct {
    volatile LONG read, published;
    RecMpPacket packet[MP_QUEUE_SLOTS];
} MpQueue;

struct RecMpChannel {
    volatile LONG refs, stop, failed, sent, sent_bytes;
    IDirectPlay2A* dp;
    DWORD origin;
    GUID session, stream;
    unsigned sequence, received, lost_at_start;
    MpQueue tx;
};

/* RX storage outlives every level and worker. Loader and game-thread receive calls can
   overlap; only producers use the try-lock. The single game-thread consumer releases a
   slot after copying it. Indices never reset, including at a level boundary. */
static MpQueue s_rx;
static volatile LONG s_rx_producer, s_lost, s_workers;

static unsigned acquire(volatile LONG* p)
{
    return (unsigned)InterlockedCompareExchange(p, 0, 0);
}

static void channel_release(RecMpChannel* c)
{
    if (!InterlockedDecrement(&c->refs)) VirtualFree(c, 0, MEM_RELEASE);
}

static DWORD WINAPI send_worker(void* arg)
{
    RecMpChannel* c = (RecMpChannel*)arg;
    unsigned r = 0;
    while (!acquire(&c->stop)) {
        RecMpPacket* p;
        HRESULT hr;
        if (r == acquire(&c->tx.published)) { Sleep(10); continue; }
        p = &c->tx.packet[r % MP_QUEUE_SLOTS];
        /* The slot and the interface are this worker's until Send returns. Even an
           engine Close or Release cannot free its AddRef; an ended session fails Send.
           No queue lock is held across a DirectPlay call. */
        hr = IDirectPlay2_Send(c->dp, c->origin, DPID_ALLPLAYERS, DPSEND_GUARANTEED,
                              p, p->h.bytes);
        if (FAILED(hr)) { InterlockedExchange(&c->failed, hr); break; }
        InterlockedIncrement(&c->sent);
        InterlockedExchangeAdd(&c->sent_bytes, p->h.bytes);
        InterlockedExchange(&c->tx.read, ++r);
    }
    IDirectPlay2_Release(c->dp);
    channel_release(c);
    InterlockedExchange(&s_workers, 0);
    return 0;
}

RecMpChannel* tagpu_recmp_begin(IDirectPlay2A* dp, DWORD origin, const GUID* session)
{
    RecMpChannel* c;
    HANDLE worker;
    if (!dp || !origin || !session || InterlockedCompareExchange(&s_workers, 1, 0)) return NULL;
    c = (RecMpChannel*)VirtualAlloc(NULL, sizeof *c, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!c) { InterlockedExchange(&s_workers, 0); return NULL; }
    if (FAILED(CoCreateGuid(&c->stream))) {
        VirtualFree(c, 0, MEM_RELEASE);
        InterlockedExchange(&s_workers, 0);
        return NULL;
    }
    c->refs = 2;
    c->dp = dp;
    c->origin = origin;
    c->session = *session;
    c->lost_at_start = acquire(&s_lost);
    IDirectPlay2_AddRef(dp);
    worker = CreateThread(NULL, 0, send_worker, c, 0, NULL);
    if (!worker) {
        IDirectPlay2_Release(dp);
        VirtualFree(c, 0, MEM_RELEASE);
        InterlockedExchange(&s_workers, 0);
        return NULL;
    }
    CloseHandle(worker);
    return c;
}

int tagpu_recmp_send(RecMpChannel* c, unsigned kind, unsigned tick,
                     const void* body, unsigned bytes)
{
    unsigned w;
    RecMpPacket* p;
    if (!c || bytes > RECMP_BODY_MAX || (bytes && !body) || acquire(&c->failed)) return 0;
    w = acquire(&c->tx.published);
    if (w - acquire(&c->tx.read) == MP_QUEUE_SLOTS) {
        InterlockedExchange(&c->failed, E_OUTOFMEMORY);
        return 0;
    }
    p = &c->tx.packet[w % MP_QUEUE_SLOTS];
    memcpy(p->h.magic, s_magic, sizeof s_magic);
    p->h.version = RECMP_VERSION;
    p->h.header = sizeof p->h;
    p->h.bytes = sizeof p->h + bytes;
    p->h.origin = c->origin;
    p->h.tick = tick;
    p->h.sequence = ++c->sequence;
    p->h.kind = kind;
    p->h.session = c->session;
    p->h.stream = c->stream;
    if (bytes) memcpy(p->body, body, bytes);
    /* Publication transfers this slot to the worker. It becomes reusable only when
       the worker publishes read, after Send returns; elapsed time has no role. */
    InterlockedExchange(&c->tx.published, w + 1);
    return 1;
}

static void receive_put(const RecMpPacket* p, unsigned bytes)
{
    unsigned w;
    if (InterlockedCompareExchange(&s_rx_producer, 1, 0)) {
        InterlockedIncrement(&s_lost);
        return;
    }
    w = acquire(&s_rx.published);
    if (w - acquire(&s_rx.read) == MP_QUEUE_SLOTS) InterlockedIncrement(&s_lost);
    else {
        memcpy(&s_rx.packet[w % MP_QUEUE_SLOTS], p, bytes);
        InterlockedExchange(&s_rx.published, w + 1);
    }
    InterlockedExchange(&s_rx_producer, 0);
}

HRESULT tagpu_recmp_receive(IDirectPlay2A* dp, DWORD* from, DWORD* to,
                            void* buffer, DWORD* bytes)
{
    unsigned i;
    DWORD capacity = *bytes;
    for (i = 0; i < MP_RECEIVE_BUDGET; i++) {
        DWORD f = 0, t = 0;
        HRESULT hr;
        *bytes = capacity;
        hr = IDirectPlay2_Receive(dp, &f, &t, DPRECEIVE_ALL, buffer, bytes);
        if (hr != DP_OK) return hr;
        /* Returned length is data until it is bounded by the supplied allocation. */
        if (*bytes > capacity) { InterlockedIncrement(&s_lost); return DPERR_INVALIDPARAMS; }
        if (f && *bytes >= sizeof s_magic && !memcmp(buffer, s_magic, sizeof s_magic)) {
            RecMpHeader h;
            if (*bytes < sizeof h || *bytes > RECMP_PACKET_MAX) {
                InterlockedIncrement(&s_lost);
                continue;
            }
            memcpy(&h, buffer, sizeof h);
            if (h.version != RECMP_VERSION || h.header != sizeof h || h.bytes != *bytes ||
                h.origin != f || !f || !h.sequence) InterlockedIncrement(&s_lost);
            else receive_put((const RecMpPacket*)buffer, *bytes);
            continue;
        }
        *from = f;
        *to = t;
        return hr;
    }
    /* Bound recorder work per pump call. Remaining messages stay on DirectPlay's
       queue; nothing is discarded and the engine can poll it again. */
    *bytes = 0;
    return DPERR_NOMESSAGES;
}

int tagpu_recmp_take(RecMpChannel* c, RecMpPacket* out)
{
    unsigned r, w;
    if (!c || !out) return 0;
    r = acquire(&s_rx.read);
    w = acquire(&s_rx.published);
    while (r != w) {
        const RecMpPacket* p = &s_rx.packet[r % MP_QUEUE_SLOTS];
        int match = !memcmp(&p->h.session, &c->session, sizeof c->session);
        if (match) memcpy(out, p, p->h.bytes);
        InterlockedExchange(&s_rx.read, ++r);
        if (match) { c->received++; return 1; }
    }
    return 0;
}

void tagpu_recmp_stats(RecMpChannel* c, RecMpStats* out)
{
    memset(out, 0, sizeof *out);
    if (!c) return;
    out->queued = acquire(&c->tx.published) - acquire(&c->tx.read);
    out->sent = acquire(&c->sent);
    out->bytes = acquire(&c->sent_bytes);
    out->received = c->received;
    out->lost = acquire(&s_lost) - c->lost_at_start;
    out->failed = (HRESULT)acquire(&c->failed);
}

void tagpu_recmp_end(RecMpChannel* c)
{
    if (!c) return;
    InterlockedExchange(&c->stop, 1);
    channel_release(c);
}
