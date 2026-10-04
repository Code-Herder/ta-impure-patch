#ifndef TAGPU_RECMP_H
#define TAGPU_RECMP_H

#include <windows.h>
#include <dplay.h>
#include <stdint.h>

/* Recorder contributions have their own framing, before the engine's compression and
   reliable-packet splitter. No engine pointer crosses into a contribution or its worker. */
#define RECMP_PACKET_MAX 4096u
#define RECMP_VERSION 1u
enum { RECMP_RESOURCES = 1, RECMP_ORDERS = 2 };

#pragma pack(push, 1)
typedef struct {
    uint8_t code;
    uint16_t version, bytes;
    uint32_t origin;
    GUID match;
} RecMpStart;
typedef struct {
    unsigned char magic[8];
    uint16_t version, header;
    uint32_t bytes, origin, tick, sequence, kind;
    GUID match, stream;
} RecMpHeader;
#pragma pack(pop)

/* The engine's 0x08 start action carries a fresh host-created match ID. Its
   delivered span, version and native sender must agree before that ID is used. */
int tagpu_recmp_start_read(const void* message, unsigned bytes, DWORD from, GUID* match);

#define RECMP_BODY_MAX (RECMP_PACKET_MAX - sizeof(RecMpHeader))
typedef struct {
    RecMpHeader h;
    unsigned char body[RECMP_BODY_MAX];
} RecMpPacket;
typedef struct {
    uint32_t player;
    unsigned char values[32];
} RecMpResources;

/* A transaction's chunks are consecutive on one source stream. Unit slots are shared
   across peers; birth is the owning peer's incarnation stamp, never an address. */
typedef struct {
    uint16_t slot;
    uint8_t list, reserved;
    uint32_t birth;
    uint16_t total, first, count, reserved2;
} RecMpOrders;

#define RECMP_ORDER_BYTES 88u
#define RECMP_ORDER_MAX 4096u
#define RECMP_ORDER_CHUNK ((RECMP_BODY_MAX - sizeof(RecMpOrders)) / RECMP_ORDER_BYTES)

typedef struct RecMpOrderCache RecMpOrderCache;
typedef struct {
    unsigned origin, birth, source_tick, sequence, applied_tick, count;
} RecMpOrderState;

/* Fixed room for 131072 nodes, two lists per slot, and at most ten assembling sources.
   All operations are owned by one game thread. A rejected chunk leaves committed lists
   intact; only a complete, bounded transaction replaces one. source_tick belongs to the
   sender, applied_tick to this recorder at publication of the final chunk. */
RecMpOrderCache* tagpu_recmp_orders_begin(unsigned slots);
void tagpu_recmp_orders_end(RecMpOrderCache* c);
int tagpu_recmp_orders_same(const RecMpOrderCache* c, unsigned slot, unsigned list,
                            unsigned origin, unsigned birth, const void* image, unsigned count);
int tagpu_recmp_orders_put(RecMpOrderCache* c, const RecMpHeader* h,
                           const void* body, unsigned bytes, unsigned applied_tick);
int tagpu_recmp_orders_get(const RecMpOrderCache* c, unsigned slot, unsigned list,
                           RecMpOrderState* state, void* image, unsigned capacity);
void tagpu_recmp_orders_drop(RecMpOrderCache* c, unsigned slot);

typedef struct RecMpChannel RecMpChannel;
typedef struct {
    unsigned queued, sent, bytes, received, lost;
    HRESULT failed;
} RecMpStats;

/* Game-thread producer. begin owns an AddRef until the worker has finished; end hands
   the channel to that worker without joining it. At most one worker exists at a time:
   a still-finishing channel makes the next begin fail, never wait or allocate another. */
RecMpChannel* tagpu_recmp_begin(IDirectPlay2A* dp, DWORD origin, const GUID* match,
                               DWORD session_flags);
int tagpu_recmp_send(RecMpChannel* c, unsigned kind, unsigned tick,
                     const void* body, unsigned bytes);
int tagpu_recmp_send_from(RecMpChannel* c, DWORD origin, unsigned kind, unsigned tick,
                          const void* body, unsigned bytes, RecMpHeader* header);
void tagpu_recmp_stats(RecMpChannel* c, RecMpStats* out);
void tagpu_recmp_end(RecMpChannel* c);

/* The receive wrapper's arguments, with its DirectPlay interface already obtained by
   the caller. Its from/to outputs are changed only for a packet returned to the engine.
   Private packets are copied to a fixed queue; a contended producer or a full queue
   drops them and reports loss rather than waiting on the game/loader thread. */
HRESULT tagpu_recmp_receive(IDirectPlay2A* dp, DWORD* from, DWORD* to,
                            void* buffer, DWORD* bytes);
/* One game-thread consumer. Returns 1 for a logical packet, 0 when empty, -1 for
   a sequence/stream failure. Duplicate deliveries and our own stream are consumed. */
int tagpu_recmp_take(RecMpChannel* c, RecMpPacket* out);

#endif
