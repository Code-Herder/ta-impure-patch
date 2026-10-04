#ifndef TAGPU_RECMP_RECORD_H
#define TAGPU_RECMP_RECORD_H

#include "tagpu_recmp_channel.h"

/* Production and consumption share one game-thread journal producer. Its writer
   reads copied bytes only. Proof tails after teardown bypass this queue and are
   recorded by the transferred consumer with no application ordinal. */
#define RECMP_JOURNAL_BYTES (16u << 20)
enum { RECMP_RECORD_PRODUCED = 1, RECMP_RECORD_CONSUMED = 2, RECMP_RECORD_PROOF = 3 };
#pragma pack(push, 1)
typedef struct {
    uint64_t ordinal;
    uint32_t tick, phase;
} RecMpRecord;
typedef struct {
    uint32_t origin, local;
} RecMpRecordActor;
typedef struct {
    uint32_t version, count, recorder, reserved;
    GUID match;
    RecMpRecordActor actor[RECMP_ACTORS];
} RecMpRecordHeader;
#pragma pack(pop)
_Static_assert(sizeof(RecMpRecord) == 16, "contribution application metadata span");
_Static_assert(sizeof(RecMpRecordHeader) == 112, "contribution roster span");

typedef struct RecMpJournal RecMpJournal;
typedef struct {
    RecMpRecord where;
    RecMpPacket packet;
} RecMpRecorded;

RecMpJournal* tagpu_recmp_record_begin(const RecMpConfig* config);
const RecMpRecordHeader* tagpu_recmp_record_header(const RecMpJournal* j);
/* One game-thread producer. Full or malformed input fails the journal without
   overwriting unread bytes. No future append can succeed after failure/end. */
int tagpu_recmp_record_put(RecMpJournal* j, const RecMpHeader* h, const void* body,
                           unsigned bytes, unsigned tick, unsigned phase);
/* One writer consumer. 1 record, 0 empty, -1 failed. */
int tagpu_recmp_record_take(RecMpJournal* j, RecMpRecorded* out);
/* End is the producer's last access. A separate writer frees only after it has
   acquired this publication and drained the journal, or discarded a failed one. */
void tagpu_recmp_record_end(RecMpJournal* j);
int tagpu_recmp_record_done(const RecMpJournal* j);
/* Claim a fresh journal once, on its producer thread, before attaching it. */
int tagpu_recmp_record_claim(RecMpJournal* j);
void tagpu_recmp_record_free(RecMpJournal* j);
/* Attach before publishing/taking any source packet. The journal's producer
   lifetime encloses the channel's game reference; the worker never accesses it. */
int tagpu_recmp_record_attach(RecMpChannel* c, RecMpJournal* j);
/* Before ending/freeing a local journal while contributions remain active.
   Game thread only; a worker never accesses this producer pointer. */
void tagpu_recmp_record_detach(RecMpChannel* c, RecMpJournal* j);

#endif
