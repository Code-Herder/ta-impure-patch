/* Exercise the real contribution queue with a DirectPlay object whose Send is held by an
   event. The engine's reference is released during that call, without a timing assumption. */
#include "tagpu_recmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

typedef struct {
    IDirectPlay2 iface;
    volatile LONG refs;
    HANDLE entered, resume, released;
    DWORD game_thread;
    int held_image_ok, off_thread, receive_at;
    RecMpPacket remote;
} Fake;

static ULONG WINAPI fake_addref(IDirectPlay2* iface)
{
    return InterlockedIncrement(&((Fake*)iface)->refs);
}

static ULONG WINAPI fake_release(IDirectPlay2* iface)
{
    Fake* f = (Fake*)iface;
    LONG refs = InterlockedDecrement(&f->refs);
    if (!refs) SetEvent(f->released);
    return refs;
}

static HRESULT WINAPI fake_send(IDirectPlay2* iface, DPID from, DPID to, DWORD flags,
                                void* data, DWORD bytes)
{
    Fake* f = (Fake*)iface;
    RecMpPacket* p = (RecMpPacket*)data;
    f->off_thread = GetCurrentThreadId() != f->game_thread;
    SetEvent(f->entered);
    CHECK(WaitForSingleObject(f->resume, 5000) == WAIT_OBJECT_0);
    f->held_image_ok = from == 11 && to == DPID_ALLPLAYERS && flags == DPSEND_GUARANTEED &&
                       bytes == sizeof p->h + 8 && p->h.origin == 11 && p->h.tick == 9 &&
                       !memcmp(p->body, "retained", 8) && f->refs == 1;
    return DP_OK;
}

static HRESULT WINAPI fake_receive(IDirectPlay2* iface, DPID* from, DPID* to, DWORD flags,
                                   void* data, DWORD* bytes)
{
    Fake* f = (Fake*)iface;
    unsigned at = f->receive_at++;
    CHECK(flags == DPRECEIVE_ALL);
    *from = 55;
    *to = 11;
    if (at == 0) {
        CHECK(*bytes >= f->remote.h.bytes);
        memcpy(data, &f->remote, f->remote.h.bytes);
        *bytes = f->remote.h.bytes;
    } else if (at == 1) {
        /* A private header whose declared size exceeds the returned packet. */
        CHECK(*bytes >= sizeof f->remote.h);
        memcpy(data, &f->remote.h, sizeof f->remote.h);
        *bytes = sizeof f->remote.h;
    } else if (at == 2) {
        CHECK(*bytes >= 3);
        memcpy(data, "TA!", 3);
        *bytes = 3;
    } else if (at == 3) {
        /* The provider claims more than the supplied allocation; no byte is read. */
        (*bytes)++;
    } else if (at == 4) {
        /* System payloads do not become recorder messages even with this prefix. */
        CHECK(*bytes >= f->remote.h.bytes);
        memcpy(data, &f->remote, f->remote.h.bytes);
        *bytes = f->remote.h.bytes;
        *from = DPID_SYSMSG;
    } else return DPERR_NOMESSAGES;
    return DP_OK;
}

int main(void)
{
    IDirectPlay2Vtbl vt = { .AddRef = fake_addref, .Release = fake_release,
                            .Send = fake_send, .Receive = fake_receive };
    Fake f;
    RecMpChannel* c;
    RecMpPacket got;
    RecMpStats stats;
    GUID session = { 1 }, other = { 2 };
    unsigned char buffer[RECMP_PACKET_MAX];
    DWORD bytes = sizeof buffer, from = 0, to = 0;
    unsigned n;
    memset(&f, 0, sizeof f);
    f.iface.lpVtbl = &vt;
    f.refs = 1;
    f.game_thread = GetCurrentThreadId();
    f.entered = CreateEvent(NULL, TRUE, FALSE, NULL);
    f.resume = CreateEvent(NULL, TRUE, FALSE, NULL);
    f.released = CreateEvent(NULL, TRUE, FALSE, NULL);
    CHECK(sizeof(RecMpHeader) == 64 && sizeof(RecMpPacket) == RECMP_PACKET_MAX);
    CHECK(f.entered && f.resume && f.released);
    memcpy(f.remote.h.magic, "IMPUREMP", 8);
    f.remote.h.version = RECMP_VERSION;
    f.remote.h.header = sizeof f.remote.h;
    f.remote.h.bytes = sizeof f.remote.h + 4;
    f.remote.h.origin = 55;
    f.remote.h.tick = 8;
    f.remote.h.sequence = 1;
    f.remote.h.kind = RECMP_RESOURCES;
    f.remote.h.session = session;
    f.remote.h.stream = other;
    memcpy(f.remote.body, "peer", 4);

    c = tagpu_recmp_begin(&f.iface, 11, &session);
    CHECK(c && f.refs == 2);
    CHECK(tagpu_recmp_receive(&f.iface, &from, &to, buffer, &bytes) == DP_OK);
    CHECK(from == 55 && to == 11 && bytes == 3 && !memcmp(buffer, "TA!", 3));
    CHECK(tagpu_recmp_take(c, &got) && got.h.origin == 55 && !memcmp(got.body, "peer", 4));
    CHECK(!tagpu_recmp_take(c, &got));
    bytes = sizeof buffer;
    CHECK(tagpu_recmp_receive(&f.iface, &from, &to, buffer, &bytes) == DPERR_INVALIDPARAMS);
    bytes = sizeof buffer;
    CHECK(tagpu_recmp_receive(&f.iface, &from, &to, buffer, &bytes) == DP_OK);
    CHECK(from == DPID_SYSMSG && bytes == f.remote.h.bytes && !memcmp(buffer, "IMPUREMP", 8));
    CHECK(!tagpu_recmp_take(c, &got));
    tagpu_recmp_stats(c, &stats);
    CHECK(stats.received == 1 && stats.lost == 2);

    CHECK(tagpu_recmp_send(c, RECMP_RESOURCES, 9, "retained", 8));
    CHECK(WaitForSingleObject(f.entered, 5000) == WAIT_OBJECT_0);
    CHECK(!tagpu_recmp_begin(&f.iface, 11, &session));
    for (n = 0; n < 256; n++)
        if (!tagpu_recmp_send(c, RECMP_RESOURCES, 10, "retained", 8)) break;
    CHECK(n == 255);
    tagpu_recmp_stats(c, &stats);
    CHECK(stats.queued == 256 && stats.failed == E_OUTOFMEMORY);
    tagpu_recmp_end(c);
    CHECK(fake_release(&f.iface) == 1);
    CHECK(WaitForSingleObject(f.released, 0) == WAIT_TIMEOUT);
    SetEvent(f.resume);
    CHECK(WaitForSingleObject(f.released, 5000) == WAIT_OBJECT_0);
    CHECK(f.off_thread && f.held_image_ok && f.refs == 0);
    puts("PASS: private framing and bounds, game packet preserved, fixed queue overflow, off-thread Send, retained buffer and interface through teardown");
    CloseHandle(f.entered);
    CloseHandle(f.resume);
    CloseHandle(f.released);
    return 0;
}
