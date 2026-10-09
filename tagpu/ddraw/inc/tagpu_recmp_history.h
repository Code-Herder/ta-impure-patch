#ifndef TAGPU_RECMP_HISTORY_H
#define TAGPU_RECMP_HISTORY_H

#include "tagpu_recmp_file.h"
#include "tagpu_recmp_units.h"
#include "tagpu_recmp_named.h"
#include "tagpu_recmp_roots.h"
#include "tagpu_recmp_explored.h"
#include "tagpu_recmp_text.h"
#include "tagpu_recmp_pov.h"

typedef struct RecMpHistory RecMpHistory;

/* Engine-free reconstruction from copied file bytes. The source caches validate
   every envelope, including proof tails. Separate application caches publish only
   produced/consumed records, so proof completion cannot replace the recorded world.
   All storage has fixed bounds and belongs to this one reader/writer. */
RecMpHistory* tagpu_recmp_history_begin(const RecMpRecordHeader* header, const RecMpRoster* roster,
                                            unsigned width, unsigned height);
void tagpu_recmp_history_end(RecMpHistory* history);
/* Invalid input poisons the history: discard it. No state may be queried afterwards. */
int tagpu_recmp_history_record(RecMpHistory* history, const RecMpRecord* where,
                               const RecMpHeader* header, const void* body, unsigned bytes);
int tagpu_recmp_history_block(RecMpHistory* history, const void* body, unsigned bytes);
const RecMpFileState* tagpu_recmp_history_ledger(const RecMpHistory* history);
int tagpu_recmp_history_footer(const RecMpHistory* history, const RecMpRecordEnd* end);
/* Player images use recorder row coordinates; source provenance remains intact.
   0 means unknown, including a list whose first complete transaction has not arrived. */
int tagpu_recmp_history_player(const RecMpHistory* history, unsigned origin, RecMpPlayerState* out);
int tagpu_recmp_history_explored(const RecMpHistory* history, unsigned origin,
                                  RecMpExploreState* out, void* image, unsigned capacity);
int tagpu_recmp_history_unit(const RecMpHistory* history, unsigned slot, RecMpUnitState* out);
int tagpu_recmp_history_roots(const RecMpHistory* history, unsigned origin, RecMpRootsState* out);
int tagpu_recmp_history_named(const RecMpHistory* history, unsigned origin, RecMpNamedState* out);
int tagpu_recmp_history_text(const RecMpHistory* history, unsigned origin, RecMpTextState* out);
/* An actor's perspective at the application tick: its last two samples, its last click and its
   last whole selection. 0 unknown, including a selection whose first transaction is partial. */
int tagpu_recmp_history_pov(const RecMpHistory* history, unsigned origin, RecMpPovState* out);
int tagpu_recmp_history_click(const RecMpHistory* history, unsigned origin, RecMpClickState* out);
int tagpu_recmp_history_select(const RecMpHistory* history, unsigned origin, RecMpSelectState* out,
                               RecSelEntry* entries, unsigned capacity);
int tagpu_recmp_history_orders(const RecMpHistory* history, unsigned slot, unsigned list,
                               RecMpOrderState* out, void* image, unsigned capacity);

#endif
