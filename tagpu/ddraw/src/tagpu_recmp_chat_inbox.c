#include "tagpu_recmp_chat_inbox.h"
#include <string.h>

enum { INBOX_CLOSED, INBOX_READY, INBOX_CLAIMED, INBOX_LIVE };
typedef struct { unsigned tick; RecMpTextEntry entry; } ChatObservation;
struct RecMpChatInbox {
    CRITICAL_SECTION lock;
    GUID match;
    DWORD host;
    unsigned phase, head, used, seen, tick, failed;
    ChatObservation pending[RECMP_TEXT_PENDING];
};

RecMpChatInbox* tagpu_recmp_chat_inbox_begin(void)
{
    RecMpChatInbox* c=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *c);
    if (c && !InitializeCriticalSectionAndSpinCount(&c->lock,0)) {
        HeapFree(GetProcessHeap(),0,c); c=NULL;
    }
    return c;
}
void tagpu_recmp_chat_inbox_end(RecMpChatInbox* c)
{
    if (!c) return;
    DeleteCriticalSection(&c->lock);
    HeapFree(GetProcessHeap(),0,c);
}
void tagpu_recmp_chat_inbox_start(RecMpChatInbox* c, const GUID* match, DWORD host)
{
    GUID next={0}, zero={0};
    if (!c) return;
    if (match) next=*match;
    EnterCriticalSection(&c->lock);
    c->match=next; c->host=host;
    c->phase=host && memcmp(&next,&zero,sizeof zero) ? INBOX_READY : INBOX_CLOSED;
    c->head=c->used=c->seen=c->tick=c->failed=0;
    LeaveCriticalSection(&c->lock);
}
int tagpu_recmp_chat_inbox_claim(RecMpChatInbox* c, GUID* match, DWORD* host)
{
    int ok;
    if (!c || !match || !host) return 0;
    EnterCriticalSection(&c->lock);
    ok=c->phase==INBOX_READY;
    if (ok) { *match=c->match; *host=c->host; c->phase=INBOX_CLAIMED; }
    LeaveCriticalSection(&c->lock);
    return ok;
}
int tagpu_recmp_chat_inbox_match(RecMpChatInbox* c, GUID* match)
{
    int ok;
    if (!c || !match) return 0;
    EnterCriticalSection(&c->lock);
    ok=c->phase!=INBOX_CLOSED;
    if (ok) *match=c->match;
    LeaveCriticalSection(&c->lock);
    return ok;
}
void tagpu_recmp_chat_inbox_fail(RecMpChatInbox* c)
{
    if (!c) return;
    EnterCriticalSection(&c->lock);
    c->failed=1;
    LeaveCriticalSection(&c->lock);
}
void tagpu_recmp_chat_inbox_level_end(RecMpChatInbox* c)
{
    if (!c) return;
    EnterCriticalSection(&c->lock);
    if (c->phase!=INBOX_READY) {
        c->phase=INBOX_CLOSED;
        c->head=c->used=c->seen=c->tick=c->failed=0;
    }
    LeaveCriticalSection(&c->lock);
}
static int append(RecMpChatInbox* c, unsigned tick, const RecMpTextEntry* entry)
{
    ChatObservation* p;
    if (c->failed) return 0;
    if (c->used==RECMP_TEXT_PENDING || (c->seen && tick<c->tick) ||
        !tagpu_recmp_text_entry_valid(entry)) {
        c->failed=1; return 0;
    }
    p=&c->pending[(c->head+c->used)%RECMP_TEXT_PENDING];
    p->tick=tick; p->entry=*entry;
    c->used++; c->seen=1; c->tick=tick;
    return 1;
}
int tagpu_recmp_chat_inbox_source(RecMpChatInbox* c, const GUID* match,
                                  unsigned tick, const RecMpTextEntry* entry)
{
    int ok;
    if (!c || !match || !entry) return 0;
    EnterCriticalSection(&c->lock);
    ok=c->phase!=INBOX_CLOSED && !memcmp(match,&c->match,sizeof *match) &&
       entry->phase==RECMP_TEXT_SOURCE && entry->source_tick==tick;
    if (ok) ok=append(c,tick,entry);
    LeaveCriticalSection(&c->lock);
    return ok;
}
int tagpu_recmp_chat_inbox_receive(RecMpChatInbox* c, DWORD from, DWORD to,
                                   const void* packet, unsigned bytes,
                                   const volatile unsigned* clock,
                                   RecMpChatWire* out, unsigned* observed_tick)
{
    RecMpChatWire wire;
    int ok;
    if (!c || !from || !to || from==to || !clock || !out || !observed_tick) return 0;
    EnterCriticalSection(&c->lock);
    ok=c->phase!=INBOX_CLOSED && tagpu_recmp_chat_wire_read(&wire,&c->match,packet,bytes);
    if (ok) {
        unsigned tick=*clock;
        RecMpTextEntry entry={ .phase=RECMP_TEXT_RECEIVED, .mode=RECMP_TEXT_MODE_UNKNOWN,
                              .origin=from, .recipient=to, .source_tick=wire.tick,
                              .display=8, .event=wire.event };
        memcpy(entry.stock,wire.stock,sizeof entry.stock);
        append(c,tick,&entry);
        *out=wire; *observed_tick=tick;
    }
    LeaveCriticalSection(&c->lock);
    return ok;
}
static int contains(const RecMpRoster* roster, unsigned origin)
{
    for (unsigned i=0;i<roster->count;i++)
        if (roster->actor[i].origin==origin) return 1;
    return 0;
}
static int drain(RecMpChatInbox* c, const GUID* match, DWORD local,
                  const RecMpRoster* roster, RecMpTextQueue* queue, int finish)
{
    int ok;
    int valid=match && queue && tagpu_recmp_roster_valid(roster) && contains(roster,local);
    if (!c) return 0;
    EnterCriticalSection(&c->lock);
    ok=valid && (c->phase==INBOX_CLAIMED || c->phase==INBOX_LIVE) && !c->failed &&
       !memcmp(match,&c->match,sizeof *match);
    while (ok && c->used) {
        const ChatObservation* p=&c->pending[c->head];
        ok=p->entry.recipient==local && contains(roster,p->entry.origin);
        for (unsigned i=0;ok && i<RECMP_ACTORS;i++)
            if (p->entry.target[i] && !contains(roster,p->entry.target[i])) ok=0;
        if (ok) ok=tagpu_recmp_text_queue_push(queue,p->tick,&p->entry);
        if (ok) { c->head=(c->head+1)%RECMP_TEXT_PENDING; c->used--; }
        else c->failed=1;
    }
    if (finish) c->phase=INBOX_CLOSED;
    else if (ok) c->phase=INBOX_LIVE;
    LeaveCriticalSection(&c->lock);
    return ok;
}
int tagpu_recmp_chat_inbox_drain(RecMpChatInbox* c, const GUID* match,
                                 DWORD local, const RecMpRoster* roster,
                                 RecMpTextQueue* queue)
{
    return drain(c,match,local,roster,queue,0);
}
int tagpu_recmp_chat_inbox_finish(RecMpChatInbox* c, const GUID* match,
                                  DWORD local, const RecMpRoster* roster,
                                  RecMpTextQueue* queue)
{
    return drain(c,match,local,roster,queue,1);
}
