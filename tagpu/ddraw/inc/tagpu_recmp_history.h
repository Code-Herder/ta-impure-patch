#ifndef TAGPU_RECMP_HISTORY_H
#define TAGPU_RECMP_HISTORY_H

#include "tagpu_recmp_file.h"

typedef struct RecMpHistory RecMpHistory;

/* Engine-free reconstruction from copied file bytes. The source caches validate
   every envelope, including proof tails. Separate application caches publish only
   produced/consumed records, so proof completion cannot replace the recorded world.
   All storage has fixed bounds and belongs to this one reader/writer. */
RecMpHistory* tagpu_recmp_history_begin(const RecMpRecordHeader* header, const RecMpRoster* roster);
void tagpu_recmp_history_end(RecMpHistory* history);
/* Invalid input poisons the history: discard it. No state may be queried afterwards. */
int tagpu_recmp_history_record(RecMpHistory* history, const RecMpRecord* where,
                               const RecMpHeader* header, const void* body, unsigned bytes);
int tagpu_recmp_history_block(RecMpHistory* history, const void* body, unsigned bytes);
const RecMpFileState* tagpu_recmp_history_ledger(const RecMpHistory* history);
/* 0 means unknown, including a list whose first complete transaction has not arrived. */
int tagpu_recmp_history_player(const RecMpHistory* history, unsigned origin, RecMpPlayerState* out);
int tagpu_recmp_history_orders(const RecMpHistory* history, unsigned slot, unsigned list,
                               RecMpOrderState* out, void* image, unsigned capacity);

#endif
