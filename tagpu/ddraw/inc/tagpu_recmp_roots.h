#ifndef TAGPU_RECMP_ROOTS_H
#define TAGPU_RECMP_ROOTS_H

#include "tagpu_recmp_players.h"

/* One existing create, death, death-end, script, reply or spray image follows this prefix. The source's
   incarnation and capture phase survive an arrival on another peer's clock.
   A birth is a monotonic slot token: repeated creates within one tick can put
   it ahead of GameTime (tagpu_patches.c:hit_next_birth). */
#pragma pack(push, 1)
typedef struct {
    uint16_t kind, bytes;
    uint8_t before_capture, reserved[3];
    uint32_t birth;
} RecMpRootsEntry;
#pragma pack(pop)
_Static_assert(sizeof(RecMpRootsEntry)==12, "owned root prefix");
#define RECMP_ROOTS_MIN 4u
#define RECMP_ROOTS_MAX (12u+REC_UNIT_IMAGE)
#define RECMP_ROOT_REPLY_KINDS 24u
#define RECMP_ROOT_LAYERS 10
#define RECMP_ROOTS_COUNT ((RECMP_BODY_MAX-4u)/(sizeof(RecMpRootsEntry)+RECMP_ROOTS_MIN))

typedef struct {
    unsigned valid, origin, source_tick, sequence, applied_tick, bytes, count;
    GUID stream;
    unsigned char body[RECMP_BODY_MAX];
} RecMpRootsState;
typedef struct RecMpRoots RecMpRoots;

/* Copied roots only: shared global unit IDs need no mapping. Create/death source
   owner rows are validated against the source roster and retained unchanged. A
   complete batch validates before publication. These last-batch queries support
   inspection; the file retains every batch in its ordered application ledger. */
RecMpRoots* tagpu_recmp_roots_begin(const RecMpRoster* roster);
void tagpu_recmp_roots_end(RecMpRoots* cache);
int tagpu_recmp_roots_put(RecMpRoots* cache, const RecMpSeatMaps* maps,
                          const RecMpHeader* header, const void* body, unsigned bytes,
                          unsigned applied_tick);
int tagpu_recmp_root_image_valid(unsigned kind, const void* image, unsigned bytes);
int tagpu_recmp_roots_get(const RecMpRoots* cache, unsigned origin, RecMpRootsState* out);

#define RECMP_ROOTS_PENDING 4096u
typedef struct RecMpRootsQueue RecMpRootsQueue;
typedef int (*RecMpRootsEmit)(void* context, unsigned origin, unsigned tick,
                             const void* body, unsigned bytes);

/* One game-thread producer and consumer. Hooks copy values into bounded slots;
   B2 drains old ticks before sealing them, then the new tick after advancing.
   Overflow or a failed emission makes the queue permanently incomplete. */
RecMpRootsQueue* tagpu_recmp_roots_queue_begin(void);
void tagpu_recmp_roots_queue_end(RecMpRootsQueue* queue);
/* Earliest copied tick: 1 present, 0 empty, -1 incomplete. Same owner as flush. */
int tagpu_recmp_roots_queue_tick(const RecMpRootsQueue* queue, unsigned* tick);
int tagpu_recmp_roots_queue_push(RecMpRootsQueue* queue, unsigned origin, unsigned tick,
                                 unsigned birth, unsigned before_capture, unsigned kind,
                                 const void* image, unsigned bytes);
int tagpu_recmp_roots_queue_flush(RecMpRootsQueue* queue, unsigned through,
                                  RecMpRootsEmit emit, void* context);

#endif
