#ifndef TAGPU_RECMP_H
#define TAGPU_RECMP_H

#include <windows.h>
#include <dplay.h>
#include <stdint.h>

/* Recorder contributions have their own application/session and framing. No engine
   pointer or primary-session interface crosses into a contribution worker. */
#define RECMP_PACKET_MAX 4096u
#define RECMP_VERSION 2u
#define RECMP_ACTORS 10u
enum { RECMP_RESOURCES = 1, RECMP_ORDERS = 2, RECMP_BIND = 3, RECMP_SEAL = 4,
       RECMP_RECEIPT = 5, RECMP_PLAYER = 6, RECMP_SEATS = 7, RECMP_UNITS = 8,
       RECMP_NAMED = 9, RECMP_ROOTS = 10 };

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
typedef struct {
    uint32_t previous, tick;
    uint64_t data_bytes;
    uint32_t crc, final;
} RecMpSeal;
typedef struct {
    uint32_t origin, sequence;
    GUID stream;
    RecMpSeal seal;
} RecMpReceipt;
#pragma pack(pop)
_Static_assert(sizeof(RecMpSeal) == 24, "source seal wire span");
_Static_assert(sizeof(RecMpReceipt) == 48, "source receipt wire span");

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
    GUID stream;
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
/* A seal cannot close a source while its last order transaction is partial. */
int tagpu_recmp_orders_idle(const RecMpOrderCache* c, unsigned origin);

typedef struct RecMpChannel RecMpChannel;
typedef struct RecMpConfig RecMpConfig;
typedef struct {
    uint32_t origin, sequence, source_tick, application_tick;
    uint64_t application_order;
    GUID stream;
} RecMpCutPoint;
typedef struct {
    unsigned queued, sent, bytes, received, lost;
    unsigned bound, expected, player, pending, completed, completed_bytes;
    unsigned seals, ended;
    unsigned cuts, receipts, receipts_sent, finished;
    HRESULT failed;
} RecMpStats;

/* Game-thread producer. begin copies a bounded native roster/address configuration;
   the worker owns its separate session. end releases the game reference without joining.
   A still-finishing worker makes the next begin fail, never wait or allocate another.
   sent counts accepted source packets (data and seals); completed includes
   identity announcements. Capture and end share the game-thread producer. */
RecMpChannel* tagpu_recmp_begin(const RecMpConfig* config);
int tagpu_recmp_send(RecMpChannel* c, unsigned kind, unsigned tick,
                     const void* body, unsigned bytes);
int tagpu_recmp_send_from(RecMpChannel* c, DWORD origin, unsigned kind, unsigned tick,
                          const void* body, unsigned bytes, RecMpHeader* header);
/* Call before capturing tick. Advancing the source clock closes the preceding
   capture tick; repeated visits to one tick may still produce data for that tick.
   Seals accompany changed prefixes and at least once per 30 advancing ticks. */
int tagpu_recmp_advance(RecMpChannel* c, unsigned tick);
/* Freeze all local producers with final prefix seals, while retaining the game
   reference/receive consumer. No further local data can be published. */
int tagpu_recmp_finish_sources(RecMpChannel* c);
/* Production/capture failure forbids a successful terminal seal. */
void tagpu_recmp_abort(RecMpChannel* c, HRESULT reason);
void tagpu_recmp_stats(RecMpChannel* c, RecMpStats* out);
/* A retained reference owns the copied receive storage after end. Transfer the
   sole consumer only after the game has called end; later takes drain proof
   tails without extending the frozen consumption cut. Retain requires an
   already owned reference. The final release frees storage and its wake handle. */
void tagpu_recmp_retain(RecMpChannel* c);
void tagpu_recmp_release(RecMpChannel* c);
void tagpu_recmp_end(RecMpChannel* c);

/* One consumer, initially the game thread and later an explicitly handed-over
   retained consumer. Returns 1 for a logical packet, 0 when empty, -1 for
   channel failure. The worker verifies native bindings, streams and sequences before
   publishing; its one receive queue has no primary-session or loader producer. */
int tagpu_recmp_take(RecMpChannel* c, RecMpPacket* out);
/* One consumer, as for take. The game supplies its actual B2 application tick;
   each consumed source packet receives a distinct recording-local ordinal. */
int tagpu_recmp_take_at(RecMpChannel* c, RecMpPacket* out, unsigned application_tick);
/* An immutable consumption cut is available only after end. A source with no
   consumed packet has sequence/order zero; it still needs a received seal. */
int tagpu_recmp_cut(RecMpChannel* c, DWORD origin, RecMpCutPoint* out);

#endif
