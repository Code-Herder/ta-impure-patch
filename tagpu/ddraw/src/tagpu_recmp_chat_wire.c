#include "tagpu_recmp_chat_wire.h"
#include <string.h>

int tagpu_recmp_chat_wire_make(RecMpChatWire* out, const GUID* match, uint64_t event,
                              unsigned tick, const void* stock, unsigned bytes)
{
    RecMpChatWire next;
    const GUID zero={0};
    if (!out || !match || !stock || bytes!=RECMP_CHAT_STOCK_BYTES || !event) return 0;
    memset(&next,0,sizeof next);
    next.match=*match;
    memcpy(next.stock,stock,sizeof next.stock);
    if (!memcmp(&next.match,&zero,sizeof zero) || next.stock[0]!=5 || !next.stock[1]) return 0;
    next.code=RECMP_CHAT_CODE; next.version=RECMP_CHAT_WIRE_VERSION;
    next.bytes=sizeof next; next.event=event; next.tick=tick;
    memcpy(out,&next,sizeof next);
    return 1;
}

int tagpu_recmp_chat_wire_read(RecMpChatWire* out, const GUID* match,
                              const void* packet, unsigned bytes)
{
    RecMpChatWire next;
    GUID expected, zero={0};
    if (!out || !match || !packet || bytes!=sizeof next) return 0;
    expected=*match;
    memcpy(&next,packet,sizeof next);
    if (!memcmp(&expected,&zero,sizeof zero) || next.code!=RECMP_CHAT_CODE ||
        next.version!=RECMP_CHAT_WIRE_VERSION || next.bytes!=sizeof next || !next.event ||
        memcmp(&next.match,&expected,sizeof expected) || next.stock[0]!=5 || !next.stock[1]) return 0;
    memcpy(out,&next,sizeof next);
    return 1;
}
