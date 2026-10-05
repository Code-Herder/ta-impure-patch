#include "tagpu_recmp_stream.h"
#include "crc32.h"
#include <string.h>

int tagpu_recmp_stream_seal(const RecMpStream* s, unsigned tick,
                            unsigned final, RecMpSeal* out)
{
    RecMpSeal seal;
    if (!s || !out || s->ended || final > 1 || s->sequence == UINT32_MAX ||
        tick < s->tick || (s->has_seal &&
        (tick < s->sealed_tick || (!final && tick == s->sealed_tick)))) return 0;
    seal.previous = s->sequence;
    seal.tick = tick;
    seal.data_bytes = s->data_bytes;
    seal.crc = s->crc;
    seal.final = final;
    *out = seal;
    return 1;
}

int tagpu_recmp_stream_put(RecMpStream* s, const RecMpHeader* h,
                           const void* body, unsigned bytes)
{
    RecMpStream next;
    if (!s || !h || (bytes && !body) || bytes > RECMP_BODY_MAX ||
        h->header != sizeof *h || h->bytes != sizeof *h + bytes ||
        s->ended || s->sequence == UINT32_MAX || h->sequence != s->sequence + 1u ||
        h->tick < s->tick) return 0;
    next = *s;
    if (h->kind == RECMP_SEAL) {
        RecMpSeal seal, expected;
        if (bytes != sizeof seal) return 0;
        memcpy(&seal, body, sizeof seal);
        if (!tagpu_recmp_stream_seal(s, h->tick, seal.final, &expected) ||
            memcmp(&seal, &expected, sizeof seal)) return 0;
        next.has_seal = 1;
        next.sealed_tick = h->tick;
        next.sealed_sequence = h->sequence;
        next.ended = seal.final;
    } else {
        if ((h->kind != RECMP_RESOURCES && h->kind != RECMP_ORDERS && h->kind != RECMP_PLAYER && h->kind != RECMP_SEATS && h->kind != RECMP_UNITS && h->kind != RECMP_NAMED) ||
            (s->has_seal && h->tick <= s->sealed_tick) ||
            UINT64_MAX - s->data_bytes < h->bytes) return 0;
        next.data_bytes += h->bytes;
        next.crc = Crc32_ComputeBuf(s->crc, h, sizeof *h);
        next.crc = Crc32_ComputeBuf(next.crc, body, bytes);
    }
    next.sequence = h->sequence;
    next.tick = h->tick;
    *s = next;
    return 1;
}
