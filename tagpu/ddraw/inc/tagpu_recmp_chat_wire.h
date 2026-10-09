#ifndef TAGPU_RECMP_CHAT_WIRE_H
#define TAGPU_RECMP_CHAT_WIRE_H

#include <windows.h>
#include <stdint.h>

#define RECMP_CHAT_CODE 0x2Bu
#define RECMP_CHAT_WIRE_VERSION 1u
#define RECMP_CHAT_STOCK_BYTES 65u

#pragma pack(push, 1)
typedef struct {
    uint8_t code, version;
    uint16_t bytes;
    GUID match;
    uint64_t event;
    uint32_t tick;
    unsigned char stock[RECMP_CHAT_STOCK_BYTES];
} RecMpChatWire;
#pragma pack(pop)
_Static_assert(sizeof(RecMpChatWire) == 97, "identified native text packet");

/* An event is keyed by match, the native transport sender and its nonzero
   serial. The original 65-byte text packet is kept byte for byte. Callers own
   the supplied spans; these functions copy before publishing an output. */
int tagpu_recmp_chat_wire_make(RecMpChatWire* out, const GUID* match, uint64_t event,
                              unsigned tick, const void* stock, unsigned bytes);
int tagpu_recmp_chat_wire_read(RecMpChatWire* out, const GUID* match,
                              const void* packet, unsigned bytes);

#endif
