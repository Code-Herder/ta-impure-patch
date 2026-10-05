#ifndef TAGPU_RECMP_CHAT_INBOX_H
#define TAGPU_RECMP_CHAT_INBOX_H

#include "tagpu_recmp_text.h"

typedef struct RecMpChatInbox RecMpChatInbox;

/* One process-owned inbox bridges loading pumps to the game producer. Its lock
   protects copied values only: no SDK, engine call, allocation, logging or
   other lock runs while it is held. End requires all callers to have retired;
   the installed adapter retains the inbox for the process lifetime. */
RecMpChatInbox* tagpu_recmp_chat_inbox_begin(void);
void tagpu_recmp_chat_inbox_end(RecMpChatInbox* inbox);
void tagpu_recmp_chat_inbox_start(RecMpChatInbox* inbox, const GUID* match, DWORD host);
int tagpu_recmp_chat_inbox_claim(RecMpChatInbox* inbox, GUID* match, DWORD* host);
int tagpu_recmp_chat_inbox_match(RecMpChatInbox* inbox, GUID* match);
void tagpu_recmp_chat_inbox_fail(RecMpChatInbox* inbox);
/* A pending start survives the loader's level reset. A claimed/live producer
   closes before its level memory is freed. */
void tagpu_recmp_chat_inbox_level_end(RecMpChatInbox* inbox);
int tagpu_recmp_chat_inbox_source(RecMpChatInbox* inbox, const GUID* match,
                                  unsigned tick, const RecMpTextEntry* entry);
/* Packet and clock remain live for this call. The caller bounds the packet by
   its delivered span and supplies the process-lifetime main GameTime field.
   Acceptance reads that clock inside publication ownership; a producer cannot
   drain/seal past an observation still waiting to publish. A valid packet
   still returns its stock image if queue exhaustion has made capture fail. */
int tagpu_recmp_chat_inbox_receive(RecMpChatInbox* inbox, DWORD from, DWORD to,
                                   const void* packet, unsigned bytes,
                                   const volatile unsigned* clock,
                                   RecMpChatWire* wire, unsigned* observed_tick);
/* The sole game consumer transfers copied entries into its own text queue
   before any source advance. Bootstrap validates IDs against its frozen roster;
   the original source/receipt ticks are never changed. */
int tagpu_recmp_chat_inbox_drain(RecMpChatInbox* inbox, const GUID* match,
                                 DWORD local, const RecMpRoster* roster,
                                 RecMpTextQueue* queue);
/* Final transfer and admission closure share one ownership interval. Receipts
   admitted before it are transferred; later callers cannot publish into a
   producer whose final source seal or level teardown has begun. */
int tagpu_recmp_chat_inbox_finish(RecMpChatInbox* inbox, const GUID* match,
                                  DWORD local, const RecMpRoster* roster,
                                  RecMpTextQueue* queue);

#endif
