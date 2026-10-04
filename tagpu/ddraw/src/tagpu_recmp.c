#include "tagpu_recmp_channel.h"
#include "tagpu_log.h"
#include <string.h>

#define MP_QUEUE_SLOTS 256u
#define MP_WORK_BUDGET 32u
static const unsigned char s_magic[8] = { 'I','M','P','U','R','E','M','P' };

typedef struct {
    volatile LONG read, published;
    RecMpPacket packet[MP_QUEUE_SLOTS];
} MpQueue;
typedef struct { RecMpHeader h; RecMpSignedIdentity body; } MpAnnouncement;

struct RecMpChannel {
    volatile LONG refs, stop, failed, sent, sent_bytes, lost;
    volatile LONG bound, player, pending, completed, completed_bytes;
    HANDLE wake;
    RecMpConfig config;
    GUID stream;
    unsigned local_mask, expected, received;
    /* tx_sequence belongs to the game thread. Bindings and last received images
       belong to the worker; the copied roster and stream never change. */
    struct {
        unsigned tx_sequence, bound;
        GUID stream;
        RecMpSignedIdentity binding;
        RecMpPacket last;
        MpAnnouncement announce;
    } actor[RECMP_ACTORS];
    MpQueue tx, rx;
};

static volatile LONG s_workers;

static unsigned acquire(volatile LONG* p)
{
    return (unsigned)InterlockedCompareExchange(p, 0, 0);
}

static void fail(RecMpChannel* c, HRESULT hr)
{
    InterlockedCompareExchange(&c->failed, hr, 0);
}

int tagpu_recmp_start_read(const void* message, unsigned bytes, DWORD from, GUID* match)
{
    RecMpStart start;
    GUID zero = { 0 };
    if (!message || !match || !from || bytes != sizeof start) return 0;
    memcpy(&start, message, sizeof start);
    if (start.code != 0x08 || start.version != RECMP_VERSION ||
        start.bytes != sizeof start || start.origin != from ||
        !memcmp(&start.match, &zero, sizeof zero)) return 0;
    *match = start.match;
    return 1;
}

static void channel_release(RecMpChannel* c)
{
    if (!InterlockedDecrement(&c->refs)) {
        /* The game can signal wake after publishing stop. Its reference keeps
           this handle alive even if the worker has already observed that store. */
        CloseHandle(c->wake);
        VirtualFree(c, 0, MEM_RELEASE);
    }
}

static void header(RecMpChannel* c, RecMpHeader* h, DWORD origin,
                    unsigned kind, unsigned tick, unsigned sequence, unsigned bytes)
{
    memcpy(h->magic, s_magic, sizeof s_magic);
    h->version = RECMP_VERSION;
    h->header = sizeof *h;
    h->bytes = sizeof *h + bytes;
    h->origin = origin;
    h->tick = tick;
    h->sequence = sequence;
    h->kind = kind;
    h->match = c->config.net.match;
    h->stream = c->stream;
}

static int announcements(RecMpChannel* c, RecMpNet* net)
{
    unsigned i;
    for (i = 0; i < c->config.count; i++) {
        MpAnnouncement* p = &c->actor[i].announce;
        if (!c->config.actor[i].local) continue;
        header(c, &p->h, c->config.actor[i].origin, RECMP_BIND, 0, 0, sizeof p->body);
        if (!tagpu_recmp_sign(c->config.signer, p->h.origin, &c->stream,
                              tagpu_recmp_net_identity(net), &p->body)) return 0;
    }
    return 1;
}

static unsigned source_of(const RecMpChannel* c, DWORD origin)
{
    unsigned i;
    for (i = 0; i < c->config.count; i++)
        if (c->config.actor[i].origin == origin) break;
    return i;
}

static int receive_packet(RecMpChannel* c, RecMpNet* net, DWORD from,
                           const RecMpPacket* p, unsigned bytes, unsigned* announce)
{
    unsigned at, w;
    RecMpPacket* last;
    if (from == tagpu_recmp_net_identity(net)->player) return 1;
    if (bytes < sizeof p->h || bytes > sizeof *p ||
        memcmp(p->h.magic, s_magic, sizeof s_magic) || p->h.version != RECMP_VERSION ||
        p->h.header != sizeof p->h || p->h.bytes != bytes) goto invalid;
    if (memcmp(&p->h.match, &c->config.net.match, sizeof p->h.match)) return 1;
    at = source_of(c, p->h.origin);
    if (at == c->config.count || c->config.actor[at].local) goto unauthorized;
    if (p->h.kind == RECMP_BIND) {
        if (p->h.sequence || p->h.tick || bytes != sizeof(MpAnnouncement)) goto invalid;
        if (c->actor[at].bound) {
            if (memcmp(&p->h.stream, &c->actor[at].stream, sizeof p->h.stream) ||
                memcmp(p->body, &c->actor[at].binding, sizeof c->actor[at].binding) ||
                !tagpu_recmp_net_authorized(net, &c->actor[at].binding.identity, from)) goto unauthorized;
            return 1;
        }
        if (!tagpu_recmp_verify(&c->config.actor[at].public, p->h.origin, &p->h.stream,
                                tagpu_recmp_net_identity(net), from, p->body,
                                bytes - sizeof p->h, &c->actor[at].binding)) goto unauthorized;
        c->actor[at].stream = p->h.stream;
        c->actor[at].bound = 1;
        InterlockedIncrement(&c->bound);
        /* A newly verified remote actor proves its process has joined. Resend
           every local identity before any later data send, including for late joiners. */
        *announce |= c->local_mask;
        tagpu_logf("recmp: bound primary ID %u to private ID %u", p->h.origin, from);
        return 1;
    }
    if (!c->actor[at].bound ||
        memcmp(&p->h.stream, &c->actor[at].stream, sizeof p->h.stream) ||
        !tagpu_recmp_net_authorized(net, &c->actor[at].binding.identity, from)) goto unauthorized;
    if (!p->h.sequence || (p->h.kind != RECMP_RESOURCES && p->h.kind != RECMP_ORDERS)) goto invalid;
    last = &c->actor[at].last;
    if (p->h.sequence < last->h.sequence) return 1;
    if (p->h.sequence == last->h.sequence) {
        if (bytes != last->h.bytes || memcmp(p, last, bytes)) goto invalid;
        return 1;
    }
    if (p->h.sequence != last->h.sequence + 1u || p->h.tick < last->h.tick) goto invalid;
    w = acquire(&c->rx.published);
    if (w - acquire(&c->rx.read) >= MP_QUEUE_SLOTS) {
        InterlockedIncrement(&c->lost);
        fail(c, E_OUTOFMEMORY);
        return 0;
    }
    memcpy(last, p, bytes);
    memcpy(&c->rx.packet[w % MP_QUEUE_SLOTS], p, bytes);
    InterlockedExchange(&c->rx.published, w + 1);
    return 1;
unauthorized:
    fail(c, E_ACCESSDENIED);
    return 0;
invalid:
    InterlockedIncrement(&c->lost);
    fail(c, E_INVALIDARG);
    return 0;
}

static void progress(RecMpChannel* c, const RecMpNetStats* stats)
{
    InterlockedExchange(&c->pending, stats->pending);
    InterlockedExchange(&c->completed, stats->completed);
    InterlockedExchange(&c->completed_bytes, stats->completed_bytes);
    if (stats->failed) fail(c, stats->failed);
}

static DWORD WINAPI worker(void* arg)
{
    RecMpChannel* c = arg;
    RecMpNet* net = NULL;
    RecMpNetStats stats = {0};
    RecMpPacket packet;
    unsigned announce = c->local_mask, membership = ~0u;
    HRESULT hr;
    do {
        if (acquire(&c->stop) || acquire(&c->failed)) {
            fail(c, E_ABORT);
            goto finish;
        }
        hr = tagpu_recmp_net_open(&c->config.net, &net);
        if (hr == DPERR_NOSESSIONS) WaitForSingleObject(c->wake, 50);
    } while (hr == DPERR_NOSESSIONS);
    if (hr != DP_OK) { fail(c, hr); goto finish; }
    InterlockedExchange(&c->player, tagpu_recmp_net_identity(net)->player);
    if (!announcements(c, net)) { fail(c, E_FAIL); goto finish; }
    tagpu_logf("recmp: isolated session open, private ID %u, expected remote actors %u",
               tagpu_recmp_net_identity(net)->player, c->expected);
    while (!acquire(&c->failed)) {
        unsigned budget, r, w;
        for (budget = 0; budget < MP_WORK_BUDGET; budget++) {
            DWORD from = 0;
            unsigned bytes = sizeof packet;
            hr = tagpu_recmp_net_receive(net, &from, &packet, &bytes);
            if (hr == DPERR_NOMESSAGES) {
                if (bytes) break;
                continue;                     /* one consumed native system notification */
            }
            if (hr != DP_OK) { fail(c, hr); break; }
            if (!receive_packet(c, net, from, &packet, bytes, &announce)) break;
        }
        tagpu_recmp_net_stats(net, &stats);
        progress(c, &stats);
        if (acquire(&c->failed)) break;
        if (membership != stats.membership) {
            membership = stats.membership;
            announce |= c->local_mask;
        }
        if (stats.peers) {
            for (budget = 0; budget < c->config.count && announce && !acquire(&c->failed); budget++) {
                if (!(announce & (1u << budget))) continue;
                hr = tagpu_recmp_net_send(net, &c->actor[budget].announce,
                                          sizeof c->actor[budget].announce);
                if (hr == DPERR_BUSY) break;
                if (hr != DP_OK) { fail(c, hr); break; }
                announce &= ~(1u << budget);
            }
        }
        r = acquire(&c->tx.read);
        w = acquire(&c->tx.published);
        if ((!announce || !c->expected) && acquire(&c->bound) == c->expected) {
            for (budget = 0; budget < MP_WORK_BUDGET && r != w && !acquire(&c->failed); budget++) {
                const RecMpPacket* p = &c->tx.packet[r % MP_QUEUE_SLOTS];
                if (c->expected) {
                    /* All expected actors have joined and every local identity
                       send precedes data on this ordered private native sender. */
                    hr = tagpu_recmp_net_send(net, p, p->h.bytes);
                    if (hr == DPERR_BUSY) break;
                    if (hr != DP_OK) { fail(c, hr); break; }
                    InterlockedIncrement(&c->sent);
                    InterlockedExchangeAdd(&c->sent_bytes, p->h.bytes);
                }
                /* SendEx copies accepted bytes; completion contexts live in net.
                   Only this publication allows the game producer to reuse the slot. */
                InterlockedExchange(&c->tx.read, ++r);
            }
        }
        tagpu_recmp_net_stats(net, &stats);
        progress(c, &stats);
        if (acquire(&c->failed)) break;
        if (acquire(&c->stop)) {
            if (acquire(&c->bound) != c->expected) { fail(c, E_ABORT); break; }
            if (r == acquire(&c->tx.published) && !stats.pending) {
                hr = tagpu_recmp_net_close(net);
                net = NULL;
                if (hr != DP_OK) fail(c, hr);
                break;
            }
        }
        if (r == acquire(&c->tx.published) || stats.pending == RECMP_NET_PENDING ||
            acquire(&c->bound) != c->expected) {
            HANDLE events[2] = {c->wake, tagpu_recmp_net_event(net)};
            WaitForMultipleObjects(2, events, FALSE, 50);
        }
    }
finish:
    if (net) tagpu_recmp_net_abort(net);
    tagpu_logf("recmp: isolated worker ended: accepted %u, completed %u, queued %u, "
               "unread %u, failed %08X; terminal stream completeness unproved",
               acquire(&c->sent), acquire(&c->completed),
               acquire(&c->tx.published) - acquire(&c->tx.read),
               acquire(&c->rx.published) - acquire(&c->rx.read), acquire(&c->failed));
    channel_release(c);
    InterlockedExchange(&s_workers, 0);
    return 0;
}

RecMpChannel* tagpu_recmp_begin(const RecMpConfig* config)
{
    RecMpChannel* c;
    RecMpPublic public, checked;
    HANDLE thread;
    GUID zero = {0};
    unsigned i, j, local_mask = 0, expected = 0, own = 0;
    if (!config || !config->signer || !config->origin || !config->count ||
        config->count > RECMP_ACTORS || config->net.host > 1 ||
        !config->net.address_bytes || config->net.address_bytes > sizeof config->net.address ||
        !memcmp(&config->net.match, &zero, sizeof zero)) return NULL;
    tagpu_recmp_public(config->signer, &public);
    for (i = 0; i < config->count; i++) {
        const RecMpActor* a = &config->actor[i];
        if (!a->origin || a->local > 1 ||
            !tagpu_recmp_public_read(&a->public, sizeof a->public, &checked)) return NULL;
        for (j = 0; j < i; j++) if (config->actor[j].origin == a->origin) return NULL;
        if (a->local) {
            if (memcmp(&a->public, &public, sizeof public)) return NULL;
            local_mask |= 1u << i;
            if (a->origin == config->origin) own = 1;
        } else expected++;
    }
    if (!own || InterlockedCompareExchange(&s_workers, 1, 0)) return NULL;
    c = VirtualAlloc(NULL, sizeof *c, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!c) goto refused;
    c->wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!c->wake || FAILED(CoCreateGuid(&c->stream))) {
        if (c->wake) CloseHandle(c->wake);
        VirtualFree(c, 0, MEM_RELEASE);
        goto refused;
    }
    c->refs = 2;
    c->config = *config;
    c->local_mask = local_mask;
    c->expected = expected;
    thread = CreateThread(NULL, 0, worker, c, 0, NULL);
    if (!thread) {
        CloseHandle(c->wake);
        VirtualFree(c, 0, MEM_RELEASE);
        goto refused;
    }
    CloseHandle(thread);
    return c;
refused:
    InterlockedExchange(&s_workers, 0);
    return NULL;
}

int tagpu_recmp_send(RecMpChannel* c, unsigned kind, unsigned tick,
                     const void* body, unsigned bytes)
{
    return tagpu_recmp_send_from(c, c ? c->config.origin : 0, kind, tick, body, bytes, NULL);
}

int tagpu_recmp_send_from(RecMpChannel* c, DWORD origin, unsigned kind, unsigned tick,
                          const void* body, unsigned bytes, RecMpHeader* out)
{
    unsigned w, at;
    RecMpPacket* p;
    if (!c || acquire(&c->stop) || acquire(&c->failed)) return 0;
    at = source_of(c, origin);
    if (at == c->config.count || !c->config.actor[at].local ||
        bytes > RECMP_BODY_MAX || (bytes && !body) ||
        (kind != RECMP_RESOURCES && kind != RECMP_ORDERS) ||
        c->actor[at].tx_sequence == ~0u) {
        fail(c, E_INVALIDARG);
        return 0;
    }
    w = acquire(&c->tx.published);
    if (w - acquire(&c->tx.read) >= MP_QUEUE_SLOTS) {
        fail(c, E_OUTOFMEMORY);
        SetEvent(c->wake);
        return 0;
    }
    p = &c->tx.packet[w % MP_QUEUE_SLOTS];
    header(c, &p->h, origin, kind, tick, ++c->actor[at].tx_sequence, bytes);
    if (bytes) memcpy(p->body, body, bytes);
    if (out) *out = p->h;
    InterlockedExchange(&c->tx.published, w + 1);
    SetEvent(c->wake);
    return 1;
}

int tagpu_recmp_take(RecMpChannel* c, RecMpPacket* out)
{
    unsigned r;
    if (!c || !out) return 0;
    if (acquire(&c->failed)) return -1;
    r = acquire(&c->rx.read);
    if (r == acquire(&c->rx.published)) return 0;
    memcpy(out, &c->rx.packet[r % MP_QUEUE_SLOTS], c->rx.packet[r % MP_QUEUE_SLOTS].h.bytes);
    InterlockedExchange(&c->rx.read, r + 1);
    c->received++;
    return 1;
}

void tagpu_recmp_stats(RecMpChannel* c, RecMpStats* out)
{
    memset(out, 0, sizeof *out);
    if (!c) return;
    out->queued = acquire(&c->tx.published) - acquire(&c->tx.read);
    out->sent = acquire(&c->sent);
    out->bytes = acquire(&c->sent_bytes);
    out->received = c->received;
    out->lost = acquire(&c->lost);
    out->bound = acquire(&c->bound);
    out->expected = c->expected;
    out->player = acquire(&c->player);
    out->pending = acquire(&c->pending);
    out->completed = acquire(&c->completed);
    out->completed_bytes = acquire(&c->completed_bytes);
    out->failed = (HRESULT)acquire(&c->failed);
}

void tagpu_recmp_end(RecMpChannel* c)
{
    if (!c) return;
    InterlockedExchange(&c->stop, 1);
    SetEvent(c->wake);
    channel_release(c);
}
