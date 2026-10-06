#ifndef TAGPU_RECMP_POV_H
#define TAGPU_RECMP_POV_H
/* tagpu_recmp_pov -- each actor's perspective as multiplayer contributions carry it: RECMP_POV
   (one RecPov, its camera and cursor), RECMP_CLICK (one RecClick) and RECMP_SELECT (a selection
   transaction of RecSel chunks), the payloads of tagpu_recpov.h
   (research/notes/tadr-port/demo-recorder-perspective-streams.md, *Multiplayer*). In a
   contribution every payload's `player` is 0xFF: the envelope's origin names the actor, and a
   reader maps it to a seat. Engine-free and copied-data only, one owner thread per cache;
   tools/replay/recmp_file.py mirrors every check. */

#include "tagpu_recmp_players.h"
#include "tagpu_recpov.h"

#define RECMP_POV_PLAYER   0xFFu
/* entries one RECMP_SELECT chunk carries: (RECMP_BODY_MAX - 8) / 6 = 670 */
#define RECMP_SELECT_CHUNK ((unsigned)((RECMP_BODY_MAX - sizeof(RecSel)) / sizeof(RecSelEntry)))

typedef struct {
    unsigned origin, source_tick, sequence, applied_tick;
    GUID stream;
    RecPov pov;
} RecMpPovSample;
/* An actor's last two camera/cursor samples, the earlier first: what the perspective clock
   (tagpu_persp.h) interpolates between, `applied_tick` its clock in a recording. */
typedef struct {
    unsigned count;
    RecMpPovSample sample[2];
} RecMpPovState;
typedef struct {
    unsigned valid, origin, source_tick, sequence, applied_tick;
    GUID stream;
    RecClick click;
} RecMpClickState;
typedef struct {
    unsigned valid, origin, source_tick, sequence, applied_tick, count;
    GUID stream;
} RecMpSelectState;
typedef struct RecMpPov RecMpPov;
typedef int (*RecMpSelectEmit)(void* ctx, unsigned origin, const void* body, unsigned bytes);

/* Fixed room for every actor of the roster: two samples, the last click, and a published and a
   staged selection of min(each, REC_SEL_MAX) entries. */
RecMpPov* tagpu_recmp_pov_begin(const RecMpRoster* roster);
void tagpu_recmp_pov_end(RecMpPov* pov);
/* One envelope of kind RECMP_POV, RECMP_CLICK or RECMP_SELECT. Its origin must have announced its
   source roster in `maps` on the same stream, and its provenance must not go back. A sample and a
   click must be exactly their payload, with RecPov's and RecClick's fields in range
   (rec_pov_ok, rec_click_ok). A selection chunk must be exactly its entries, its total at most
   the actor's block and REC_SEL_MAX, its slots strictly ascending across the transaction and
   inside the actor's block, and contiguous with the chunk before it on the stream: BEGIN starts
   at entry 0 with nothing staged, each further chunk takes the next sequence at the same source
   tick, and only END publishes. Refusal leaves the cache unchanged. */
int tagpu_recmp_pov_put(RecMpPov* pov, const RecMpSeatMaps* maps, const RecMpHeader* header,
                        const void* body, unsigned bytes, unsigned applied_tick);
/* A seal cannot close a source while its last selection transaction is partial. */
int tagpu_recmp_pov_idle(const RecMpPov* pov, unsigned origin);
/* 1 known, 0 unknown, the output unchanged then. */
int tagpu_recmp_pov_get(const RecMpPov* pov, unsigned origin, RecMpPovState* out);
int tagpu_recmp_pov_click(const RecMpPov* pov, unsigned origin, RecMpClickState* out);
/* A NULL `entries` asks for the metadata only; otherwise `capacity` must hold every entry. */
int tagpu_recmp_pov_select(const RecMpPov* pov, unsigned origin, RecMpSelectState* out,
                           RecSelEntry* entries, unsigned capacity);
/* THE SAMPLER's rule for a camera/cursor sample `now` taken at source tick `tick`: 1 send it,
   0 skip this tick, -1 an origin outside the roster. Skipped while the actor's last sample is
   fewer than now->every ticks old or equal to `now`, and while `room` -- the transmit queue's
   free slots (tagpu_recmp_room) -- is below RECMP_POV_ROOM: the contract allows a lost sample
   and a full queue fails every contribution, and the last sample stays as it was, so the next
   eligible tick sends the change. */
int tagpu_recmp_pov_due(const RecMpPov* pov, unsigned origin, const RecPov* now, unsigned tick,
                        unsigned room);
/* The actor's whole selection, `count` ascending entries, as one transaction of chunks of at most
   RECMP_SELECT_CHUNK entries, BEGIN to END. The entries are checked as put checks them before
   anything is emitted. emit must publish each body and put it into this same cache before it
   returns; a refusal ends the producer. 1 when the cache's published selection is the input. */
int tagpu_recmp_select_encode(RecMpPov* pov, unsigned origin, const RecSelEntry* entries,
                              unsigned count, RecMpSelectEmit emit, void* ctx);

#endif
