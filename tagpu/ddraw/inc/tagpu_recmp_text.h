#ifndef TAGPU_RECMP_TEXT_H
#define TAGPU_RECMP_TEXT_H

#include "tagpu_recmp_players.h"
#include "tagpu_recmp_chat_wire.h"

enum { RECMP_TEXT_SOURCE=1, RECMP_TEXT_RECEIVED=2, RECMP_TEXT_MODE_UNKNOWN=255 };
#pragma pack(push, 1)
typedef struct {
    uint8_t phase, mode;
    uint16_t reserved;
    uint32_t origin, recipient, source_tick, display;
    uint64_t event;
    uint32_t target[RECMP_ACTORS];
    unsigned char stock[RECMP_CHAT_STOCK_BYTES], padding[3];
} RecMpTextEntry;
#pragma pack(pop)
_Static_assert(sizeof(RecMpTextEntry)==136, "text observation span");
#define RECMP_TEXT_COUNT ((RECMP_BODY_MAX-4u)/sizeof(RecMpTextEntry))
#define RECMP_TEXT_EVENTS 65536u
#define RECMP_TEXT_PENDING 4096u

typedef struct {
    unsigned valid, origin, source_tick, sequence, applied_tick, bytes, count;
    GUID stream;
    unsigned char body[RECMP_BODY_MAX];
} RecMpTextState;
typedef struct RecMpText RecMpText;

/* SOURCE includes the formatter's local echo and its native target calls;
   mode zero represents the generic audience, with an empty target list.
   RECEIVED belongs to the recipient's stream and clock. Its unknown mode and
   empty target list do not invent the sender's intent. A receipt may precede
   its original contribution; matching uses identity and all 65 packet bytes.
   Repeated receipts are retained, never deduplicated into one display event. */
int tagpu_recmp_text_entry_valid(const RecMpTextEntry* entry);
RecMpText* tagpu_recmp_text_begin(const RecMpRoster* roster);
void tagpu_recmp_text_end(RecMpText* text);
/* Refusal leaves committed state unchanged. Identity storage has a fixed bound;
   exhaustion refuses the recording instead of dropping a communication event. */
int tagpu_recmp_text_put(RecMpText* text, const RecMpSeatMaps* maps,
                         const RecMpHeader* header, const void* body, unsigned bytes,
                         unsigned applied_tick);
/* Last-batch inspection; the ordered file retains every observation. */
int tagpu_recmp_text_get(const RecMpText* text, unsigned origin, RecMpTextState* out);
/* Complete source proof must contain an original for every claimed receipt. */
int tagpu_recmp_text_complete(const RecMpText* text);

typedef struct RecMpTextQueue RecMpTextQueue;
typedef int (*RecMpTextEmit)(void* context, unsigned origin, unsigned tick,
                             const void* body, unsigned bytes);
/* One game-thread owner. Copy at the engine boundary and drain old ticks before
   sealing them. Overflow or failed publication makes this queue incomplete. */
RecMpTextQueue* tagpu_recmp_text_queue_begin(void);
void tagpu_recmp_text_queue_end(RecMpTextQueue* queue);
int tagpu_recmp_text_queue_tick(const RecMpTextQueue* queue, unsigned* tick);
int tagpu_recmp_text_queue_push(RecMpTextQueue* queue, unsigned tick,
                                const RecMpTextEntry* entry);
int tagpu_recmp_text_queue_flush(RecMpTextQueue* queue, unsigned through,
                                 RecMpTextEmit emit, void* context);

#endif
