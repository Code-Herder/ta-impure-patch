#ifndef TAGPU_RECMP_LIFECYCLE_H
#define TAGPU_RECMP_LIFECYCLE_H

#include "tagpu_recmp_file.h"
#include "tagpu_recmp_roots.h"
#include "tagpu_recmp_units.h"

enum { RECMP_LIFE_UNKNOWN, RECMP_LIFE_ABSENT, RECMP_LIFE_CREATING,
       RECMP_LIFE_LIVE, RECMP_LIFE_DYING };
enum { RECMP_LIFE_BOOTSTRAP = 1, RECMP_LIFE_CREATE_BEGIN, RECMP_LIFE_CREATE_END,
       RECMP_LIFE_DEATH_BEGIN, RECMP_LIFE_DEATH_END, RECMP_LIFE_IMAGE,
       RECMP_LIFE_ABSENCE, RECMP_LIFE_ROOT };

typedef struct {
    unsigned phase, origin, birth, type;
} RecMpLifeState;

typedef struct {
    RecMpRecord where;
    RecMpHeader source;
    unsigned action, index, slot, kind, birth, before_capture, bytes;
    unsigned char image[RECMP_ROOTS_MAX];
} RecMpLifeAction;
typedef struct RecMpLifecycle RecMpLifecycle;
typedef int (*RecMpLifeVisit)(void* context, const RecMpLifeAction* action);

/* One owner, copied data only. Feed the semantically validated application
   envelopes supplied by recr_mp_apply, from ordinal one after each fresh load
   or seek. This consumer independently checks envelope/source ordering and the
   seat/unit/root bodies; other feature bodies belong to their own consumers.
   The definition count bounds every type before a callback can use it. */
RecMpLifecycle* tagpu_recmp_lifecycle_begin(const RecMpRecordHeader* header,
                                            const RecMpRoster* roster, unsigned definitions);
void tagpu_recmp_lifecycle_end(RecMpLifecycle* life);
/* Validate the entire copied batch and every incarnation dependency before any
   callback. Actions preserve both clocks, source sequence and entry order;
   proof tails validate source bytes but emit nothing and establish no lifetime.
   Callbacks must not call this API and must copy anything they retain. A failed
   callback permanently stops this consumer: already applied effects are never
   retried. The caller stops engine playback on any refusal. */
int tagpu_recmp_lifecycle_record(RecMpLifecycle* life, const RecMpRecord* where,
                                  const RecMpHeader* header, const void* body, unsigned bytes,
                                  RecMpLifeVisit visit, void* context);
/* 1 known, 0 unknown, -1 failed/invalid; refusal leaves out unchanged. */
int tagpu_recmp_lifecycle_get(const RecMpLifecycle* life, unsigned slot, RecMpLifeState* out);
const char* tagpu_recmp_lifecycle_error(const RecMpLifecycle* life);
/* Every owned slot needs an applied baseline and every started bracket an end.
   A proof-only image cannot close either gap. The file footer is also checked. */
int tagpu_recmp_lifecycle_complete(const RecMpLifecycle* life, const RecMpRecordEnd* end);

/* BOOTSTRAP and IMAGE carry normalized REC_UNIT_IMAGE bytes; ABSENCE carries
   none. CREATE_BEGIN/END carry the normalized constructor images. BEGIN starts
   the lifetime before private roots; END confirms the first 24 argument bytes
   and supplies the success water value, which is state rather than an argument.
   DEATH_BEGIN keeps the target available for children. DEATH_END carries the
   entry's copied, normalized 340-byte death image, for one native destruction
   after those children, with END's own provenance. The engine adapter must
   suppress native regeneration of carried child roots. This API performs no
   construction, destruction, engine write, or native child suppression. */

#endif
