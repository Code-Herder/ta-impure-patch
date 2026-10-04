#ifndef TAGPU_RECMP_H
#define TAGPU_RECMP_H

#include <windows.h>
#include <dplay.h>
#include <stdint.h>

/* Recorder contributions have their own framing, before the engine's compression and
   reliable-packet splitter. No engine pointer crosses into a contribution or its worker. */
#define RECMP_PACKET_MAX 4096u
#define RECMP_VERSION 1u
enum { RECMP_RESOURCES = 1 };

#pragma pack(push, 1)
typedef struct {
    unsigned char magic[8];
    uint16_t version, header;
    uint32_t bytes, origin, tick, sequence, kind;
    GUID session, stream;
} RecMpHeader;
#pragma pack(pop)

#define RECMP_BODY_MAX (RECMP_PACKET_MAX - sizeof(RecMpHeader))
typedef struct {
    RecMpHeader h;
    unsigned char body[RECMP_BODY_MAX];
} RecMpPacket;
typedef struct {
    uint32_t player;
    unsigned char values[32];
} RecMpResources;

typedef struct RecMpChannel RecMpChannel;
typedef struct {
    unsigned queued, sent, bytes, received, lost;
    HRESULT failed;
} RecMpStats;

/* Game-thread producer. begin owns an AddRef until the worker has finished; end hands
   the channel to that worker without joining it. At most one worker exists at a time:
   a still-finishing channel makes the next begin fail, never wait or allocate another. */
RecMpChannel* tagpu_recmp_begin(IDirectPlay2A* dp, DWORD origin, const GUID* session);
int tagpu_recmp_send(RecMpChannel* c, unsigned kind, unsigned tick,
                     const void* body, unsigned bytes);
void tagpu_recmp_stats(RecMpChannel* c, RecMpStats* out);
void tagpu_recmp_end(RecMpChannel* c);

/* The receive wrapper's arguments, with its DirectPlay interface already obtained by
   the caller. Its from/to outputs are changed only for a packet returned to the engine.
   Private packets are copied to a fixed queue; a contended producer or a full queue
   drops them and reports loss rather than waiting on the game/loader thread. */
HRESULT tagpu_recmp_receive(IDirectPlay2A* dp, DWORD* from, DWORD* to,
                            void* buffer, DWORD* bytes);
/* One game-thread consumer; session is checked before a packet leaves the queue. */
int tagpu_recmp_take(RecMpChannel* c, RecMpPacket* out);

#endif
