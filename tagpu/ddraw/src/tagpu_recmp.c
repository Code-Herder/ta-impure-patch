#include "tagpu_recmp_channel.h"
#include "tagpu_recmp_stream.h"
#include "tagpu_recmp_record.h"
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
typedef struct { RecMpHeader h; RecMpReceipt body; } MpReceipt;

struct RecMpChannel {
    volatile LONG refs, stop, failed, sent, sent_bytes, lost;
    volatile LONG bound, player, pending, completed, completed_bytes;
    volatile LONG seals, ended;
    volatile LONG cuts, receipts, receipts_sent, finished;
    HANDLE wake;
    RecMpConfig config;
    GUID stream;
    unsigned local_mask, expected, received;
    unsigned capture_tick, capture_seen, producers_closed;
    uint64_t application_order;
    RecMpJournal* journal;          /* game producer only; the worker never accesses this */
    unsigned journal_tick;
    /* tx and consumed belong to the game thread. stop publishes their immutable
       final values. rx, receipts, bindings and last received images
       belong to the worker; the copied roster and stream never change. */
    struct {
        RecMpStream tx, rx;
        unsigned bound;
        RecMpCutPoint consumed;
        unsigned receipt_pending, receipt_mask, final_sent;
        RecMpReceipt final;
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

void tagpu_recmp_retain(RecMpChannel* c)
{
    if (c) InterlockedIncrement(&c->refs);
}

void tagpu_recmp_release(RecMpChannel* c)
{
    if (!c) return;
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
    RecMpStream next;
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
    if (p->h.kind == RECMP_RECEIPT) {
        RecMpReceipt receipt;
        unsigned source;
        if (p->h.sequence || p->h.tick || bytes != sizeof(MpReceipt)) goto invalid;
        memcpy(&receipt, p->body, sizeof receipt);
        source = source_of(c, receipt.origin);
        if (source == c->config.count) goto invalid;
        /* Broadcast receipts for another producer have no effect here. The
           sender is still authenticated before this target is inspected. */
        if (!c->config.actor[source].local) return 1;
        if (!c->actor[source].final_sent ||
            memcmp(&receipt, &c->actor[source].final, sizeof receipt)) goto invalid;
        if (!(c->actor[source].receipt_mask & (1u << at))) {
            c->actor[source].receipt_mask |= 1u << at;
            InterlockedIncrement(&c->receipts);
        }
        return 1;
    }
    if (!p->h.sequence || (p->h.kind != RECMP_RESOURCES && p->h.kind != RECMP_ORDERS && p->h.kind != RECMP_PLAYER && p->h.kind != RECMP_SEATS && p->h.kind != RECMP_UNITS && p->h.kind != RECMP_NAMED && p->h.kind != RECMP_ROOTS && p->h.kind != RECMP_EXPLORED && p->h.kind != RECMP_TEXT &&
        p->h.kind != RECMP_SEAL)) goto invalid;
    last = &c->actor[at].last;
    if (p->h.sequence < last->h.sequence) return 1;
    if (p->h.sequence == last->h.sequence) {
        if (bytes != last->h.bytes || memcmp(p, last, bytes)) goto invalid;
        return 1;
    }
    next = c->actor[at].rx;
    if (!tagpu_recmp_stream_put(&next, &p->h, p->body, bytes - sizeof p->h)) goto invalid;
    w = acquire(&c->rx.published);
    if (w - acquire(&c->rx.read) >= MP_QUEUE_SLOTS) {
        InterlockedIncrement(&c->lost);
        fail(c, E_OUTOFMEMORY);
        return 0;
    }
    memcpy(last, p, bytes);
    memcpy(&c->rx.packet[w % MP_QUEUE_SLOTS], p, bytes);
    c->actor[at].rx = next;
    InterlockedExchange(&c->rx.published, w + 1);
    if (p->h.kind == RECMP_SEAL) {
        InterlockedIncrement(&c->seals);
        if (next.ended) {
            c->actor[at].receipt_pending = c->local_mask;
            InterlockedIncrement(&c->ended);
            tagpu_logf("recmp: source ID %u sealed its final prefix at tick %u seq %u, %llu data bytes",
                       p->h.origin, p->h.tick, p->h.sequence,
                       (unsigned long long)next.data_bytes);
        }
    }
    return 1;
unauthorized:
    fail(c, E_ACCESSDENIED);
    return 0;
invalid:
    InterlockedIncrement(&c->lost);
    fail(c, E_INVALIDARG);
    return 0;
}

static void final_sent(RecMpChannel* c, unsigned at, const RecMpPacket* p)
{
    RecMpSeal seal;
    if (p->h.kind != RECMP_SEAL) return;
    memcpy(&seal, p->body, sizeof seal);
    if (!seal.final) return;
    c->actor[at].final.origin = p->h.origin;
    c->actor[at].final.sequence = p->h.sequence;
    c->actor[at].final.stream = p->h.stream;
    c->actor[at].final.seal = seal;
    c->actor[at].final_sent = 1;
}

static int send_receipts(RecMpChannel* c, RecMpNet* net, unsigned no_recipients)
{
    unsigned i, j;
    for (i = 0; i < c->config.count; i++) {
        if (no_recipients) { c->actor[i].receipt_pending = 0; continue; }
        for (j = 0; j < c->config.count; j++) {
            MpReceipt p;
            HRESULT hr;
            if (!(c->actor[i].receipt_pending & (1u << j))) continue;
            header(c, &p.h, c->config.actor[j].origin, RECMP_RECEIPT, 0, 0, sizeof p.body);
            p.body.origin = c->config.actor[i].origin;
            p.body.sequence = c->actor[i].last.h.sequence;
            p.body.stream = c->actor[i].stream;
            memcpy(&p.body.seal, c->actor[i].last.body, sizeof p.body.seal);
            hr = tagpu_recmp_net_send(net, &p, sizeof p);
            if (hr == DPERR_BUSY) return 1;
            if (hr != DP_OK) { fail(c, hr); return 0; }
            c->actor[i].receipt_pending &= ~(1u << j);
            InterlockedIncrement(&c->receipts_sent);
        }
    }
    return 1;
}

/* Only the worker reads these ledgers. The consumed sequence is immutable once
   it acquires stop; a retained file consumer cannot extend this recording cut. */
static int terminal_ready(RecMpChannel* c)
{
    unsigned i, active = 0, cuts = 0, ready = 1;
    for (i = 0; i < c->config.count; i++) {
        if (c->config.actor[i].local) continue;
        if (!c->actor[i].rx.ended) active |= 1u << i;
        if (c->actor[i].rx.has_seal &&
            c->actor[i].rx.sealed_sequence >= c->actor[i].consumed.sequence) cuts++;
        if (c->actor[i].receipt_pending) ready = 0;
    }
    InterlockedExchange(&c->cuts, cuts);
    for (i = 0; i < c->config.count; i++) {
        if (!c->config.actor[i].local) continue;
        if (!c->actor[i].final_sent || (c->actor[i].receipt_mask & active) != active) ready = 0;
    }
    return ready && cuts == c->expected;
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
    unsigned stopping = 0;
    DWORD stopping_at = 0;
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
        unsigned budget, r, w, no_recipients;
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
        /* A departed private peer cannot receive another packet. Retirement is
           allowed only after every expected source supplied its final prefix;
           an absent unsealed source cannot make queued work disappear. */
        no_recipients = !stats.peers && acquire(&c->ended) == c->expected;
        if (!announce || no_recipients) send_receipts(c, net, no_recipients);
        if ((!announce || no_recipients) && acquire(&c->bound) == c->expected) {
            for (budget = 0; budget < MP_WORK_BUDGET && r != w && !acquire(&c->failed); budget++) {
                const RecMpPacket* p = &c->tx.packet[r % MP_QUEUE_SLOTS];
                if (!no_recipients) {
                    /* All expected actors have joined and every local identity
                       send precedes data on this ordered private native sender. */
                    hr = tagpu_recmp_net_send(net, p, p->h.bytes);
                    if (hr == DPERR_BUSY) break;
                    if (hr != DP_OK) { fail(c, hr); break; }
                    InterlockedIncrement(&c->sent);
                    InterlockedExchangeAdd(&c->sent_bytes, p->h.bytes);
                }
                final_sent(c, source_of(c, p->h.origin), p);
                /* SendEx copies accepted bytes; completion contexts live in net.
                   Only this publication allows the game producer to reuse the slot. */
                InterlockedExchange(&c->tx.read, ++r);
            }
        }
        tagpu_recmp_net_stats(net, &stats);
        progress(c, &stats);
        if (acquire(&c->failed)) break;
        if (acquire(&c->stop)) {
            if (!stopping) { stopping = 1; stopping_at = GetTickCount(); }
            if (acquire(&c->bound) != c->expected) { fail(c, E_ABORT); break; }
            if (r == acquire(&c->tx.published) && !stats.pending && terminal_ready(c)) {
                hr = tagpu_recmp_net_close(net);
                net = NULL;
                if (hr != DP_OK) fail(c, hr);
                break;
            }
            /* A deadline bounds abandoned drain work; it only declares failure.
               Receipt, prefix validity and delivery success never come from time. */
            if (GetTickCount() - stopping_at >= 15000u) {
                tagpu_logf("recmp: source drain deadline: queued %u pending %u final sources %u/%u cuts %u/%u receipts %u: incomplete",
                           acquire(&c->tx.published) - r, stats.pending,
                           acquire(&c->ended), c->expected, acquire(&c->cuts), c->expected,
                           acquire(&c->receipts));
                fail(c, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
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
               "unread %u, failed %08X, final sources %u/%u, cuts %u/%u, receipts %u/%u; file completeness unproved",
               acquire(&c->sent), acquire(&c->completed),
               acquire(&c->tx.published) - acquire(&c->tx.read),
               acquire(&c->rx.published) - acquire(&c->rx.read), acquire(&c->failed),
               acquire(&c->ended), c->expected, acquire(&c->cuts), c->expected,
               acquire(&c->receipts), acquire(&c->receipts_sent));
    InterlockedExchange(&c->finished, 1);
    tagpu_recmp_release(c);
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

static int publish(RecMpChannel* c, unsigned at, unsigned kind, unsigned tick,
                    const void* body, unsigned bytes, RecMpHeader* out)
{
    unsigned w;
    RecMpPacket* p;
    w = acquire(&c->tx.published);
    if (w - acquire(&c->tx.read) >= MP_QUEUE_SLOTS) {
        fail(c, E_OUTOFMEMORY);
        SetEvent(c->wake);
        return 0;
    }
    p = &c->tx.packet[w % MP_QUEUE_SLOTS];
    header(c, &p->h, c->config.actor[at].origin, kind, tick,
            c->actor[at].tx.sequence + 1u, bytes);
    if (!tagpu_recmp_stream_put(&c->actor[at].tx, &p->h, body, bytes)) {
        fail(c, E_INVALIDARG);
        SetEvent(c->wake);
        return 0;
    }
    if (bytes) memcpy(p->body, body, bytes);
    if (c->journal && !tagpu_recmp_record_put(c->journal, &p->h, p->body, bytes,
        c->capture_seen ? c->journal_tick : tick, RECMP_RECORD_PRODUCED)) {
        /* Saving a local file cannot stop mandatory contributions to
           other recorders. The failed journal persists until its hand-over. */
        c->journal = NULL;
    }
    if (out) *out = p->h;
    InterlockedExchange(&c->tx.published, w + 1);
    SetEvent(c->wake);
    return 1;
}

int tagpu_recmp_send_from(RecMpChannel* c, DWORD origin, unsigned kind, unsigned tick,
                          const void* body, unsigned bytes, RecMpHeader* out)
{
    unsigned at;
    if (!c || acquire(&c->stop) || acquire(&c->failed)) return 0;
    at = source_of(c, origin);
    if (c->producers_closed || at == c->config.count || !c->config.actor[at].local ||
        bytes > RECMP_BODY_MAX || (bytes && !body) ||
        (c->capture_seen && tick != c->capture_tick) ||
        (kind != RECMP_RESOURCES && kind != RECMP_ORDERS && kind != RECMP_PLAYER && kind != RECMP_SEATS && kind != RECMP_UNITS && kind != RECMP_NAMED && kind != RECMP_ROOTS && kind != RECMP_EXPLORED && kind != RECMP_TEXT)) {
        fail(c, E_INVALIDARG);
        SetEvent(c->wake);
        return 0;
    }
    return publish(c, at, kind, tick, body, bytes, out);
}

int tagpu_recmp_advance(RecMpChannel* c, unsigned tick)
{
    unsigned i;
    if (!c || acquire(&c->stop) || acquire(&c->failed)) return 0;
    if (c->producers_closed || (c->capture_seen && tick < c->capture_tick)) goto invalid;
    c->journal_tick = tick;
    if (c->capture_seen && tick != c->capture_tick) {
        for (i = 0; i < c->config.count; i++) {
            const RecMpStream* s = &c->actor[i].tx;
            RecMpSeal seal;
            if (!c->config.actor[i].local) continue;
            if (s->has_seal && s->sequence == s->sealed_sequence &&
                c->capture_tick - s->sealed_tick < 30u) continue;
            if (!tagpu_recmp_stream_seal(s, c->capture_tick, 0, &seal)) goto invalid;
            if (!publish(c, i, RECMP_SEAL, c->capture_tick, &seal, sizeof seal, NULL)) return 0;
        }
    }
    c->capture_tick = tick;
    c->capture_seen = 1;
    return 1;
invalid:
    fail(c, E_INVALIDARG);
    SetEvent(c->wake);
    return 0;
}

int tagpu_recmp_finish_sources(RecMpChannel* c)
{
    unsigned i;
    if (!c || acquire(&c->stop) || acquire(&c->failed)) return 0;
    if (c->producers_closed) return 1;
    c->producers_closed = 1;
    for (i = 0; i < c->config.count; i++) {
        const RecMpStream* s = &c->actor[i].tx;
        RecMpSeal seal;
        unsigned tick = c->capture_seen ? c->capture_tick : s->tick;
        if (!c->config.actor[i].local) continue;
        if (!tagpu_recmp_stream_seal(s, tick, 1, &seal)) {
            tagpu_recmp_abort(c, E_INVALIDARG);
            return 0;
        }
        if (!publish(c, i, RECMP_SEAL, tick, &seal, sizeof seal, NULL)) return 0;
    }
    return 1;
}

void tagpu_recmp_abort(RecMpChannel* c, HRESULT reason)
{
    if (!c) return;
    fail(c, FAILED(reason) ? reason : E_ABORT);
    SetEvent(c->wake);
}

int tagpu_recmp_take(RecMpChannel* c, RecMpPacket* out)
{
    return tagpu_recmp_take_at(c, out, 0);
}

int tagpu_recmp_take_at(RecMpChannel* c, RecMpPacket* out, unsigned application_tick)
{
    unsigned r, stopped;
    if (!c || !out) return 0;
    if (acquire(&c->failed)) return -1;
    r = acquire(&c->rx.read);
    if (r == acquire(&c->rx.published)) return 0;
    stopped = acquire(&c->stop);
    if (!stopped && c->application_order == UINT64_MAX) {
        tagpu_recmp_abort(c, E_INVALIDARG);
        return -1;
    }
    memcpy(out, &c->rx.packet[r % MP_QUEUE_SLOTS], c->rx.packet[r % MP_QUEUE_SLOTS].h.bytes);
    if (!stopped && c->journal && !tagpu_recmp_record_put(c->journal, &out->h, out->body,
        out->h.bytes - sizeof out->h, application_tick, RECMP_RECORD_CONSUMED)) {
        c->journal = NULL;
    }
    if (!stopped) {
        RecMpCutPoint* cut = &c->actor[source_of(c, out->h.origin)].consumed;
        cut->origin = out->h.origin;
        cut->sequence = out->h.sequence;
        cut->source_tick = out->h.tick;
        cut->application_tick = application_tick;
        cut->application_order = ++c->application_order;
        cut->stream = out->h.stream;
    }
    InterlockedExchange(&c->rx.read, r + 1);
    c->received++;
    return 1;
}

int tagpu_recmp_cut(RecMpChannel* c, DWORD origin, RecMpCutPoint* out)
{
    unsigned at;
    if (!c || !out || !acquire(&c->stop)) return 0;
    at = source_of(c, origin);
    if (at == c->config.count || c->config.actor[at].local) return 0;
    *out = c->actor[at].consumed;
    out->origin = origin;
    return 1;
}

int tagpu_recmp_record_attach(RecMpChannel* c, RecMpJournal* j)
{
    const RecMpRecordHeader* h = tagpu_recmp_record_header(j);
    unsigned i;
    if (!c || !h || c->journal || c->received || acquire(&c->tx.published) ||
        acquire(&c->stop) || acquire(&c->failed) || h->count != c->config.count ||
        h->recorder != c->config.origin || memcmp(&h->match, &c->config.net.match, sizeof h->match)) return 0;
    for (i = 0; i < h->count; i++)
        if (h->actor[i].origin != c->config.actor[i].origin ||
            h->actor[i].local != c->config.actor[i].local) return 0;
    if (!tagpu_recmp_record_claim(j)) return 0;
    c->journal = j;
    return 1;
}

void tagpu_recmp_record_detach(RecMpChannel* c, RecMpJournal* j)
{
    if (c && c->journal == j) c->journal = NULL;
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
    out->seals = acquire(&c->seals);
    out->ended = acquire(&c->ended);
    out->cuts = acquire(&c->cuts);
    out->receipts = acquire(&c->receipts);
    out->receipts_sent = acquire(&c->receipts_sent);
    out->finished = acquire(&c->finished);
    out->failed = (HRESULT)acquire(&c->failed);
}

void tagpu_recmp_end(RecMpChannel* c)
{
    if (!c) return;
    /* Capture and the engine's teardown state handlers own one game thread
       (tagpu_reclaim.c:reclaim_teardown_pre), so final publication cannot
       overlap another producer. The worker reads only copied queue slots. */
    if (!acquire(&c->failed)) tagpu_recmp_finish_sources(c);
    InterlockedExchange(&c->stop, 1);
    SetEvent(c->wake);
    tagpu_recmp_release(c);
}
