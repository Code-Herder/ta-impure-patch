#ifndef TAGPU_RECMP_STREAM_H
#define TAGPU_RECMP_STREAM_H

#include "tagpu_recmp.h"

/* One owner per ledger: the local producer or the private receive worker.
   A received seal proves the exact contiguous prefix of this source, not a
   durable file or a mapping between two peers' clocks. */
typedef struct {
    uint64_t data_bytes;
    uint32_t sequence, tick, crc, sealed_tick, sealed_sequence, has_seal, ended;
} RecMpStream;

/* Failure leaves the ledger/output unchanged. Authorization and match/stream
   identity precede this check at the channel boundary. Duplicates are removed
   there; only a new logical packet reaches the ledger. */
int tagpu_recmp_stream_put(RecMpStream* s, const RecMpHeader* h,
                           const void* body, unsigned bytes);
int tagpu_recmp_stream_seal(const RecMpStream* s, unsigned tick,
                            unsigned final, RecMpSeal* out);

#endif
