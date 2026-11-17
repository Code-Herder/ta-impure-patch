#ifndef IMPAI_REACT_H
#define IMPAI_REACT_H
/* impai_react.h -- how a level carries out ImpAI's decisions (impai_react.c): the reaction's view and its queue,
   and each manager's budget. The persona, its rows and the queue's records are impai.h's, beside IMPAI_MIL, which
   records them. Engine-free. */
#include "impai.h"

/* What the reaction did, for the glue's `impai: react` line: entries given, given past their due tick, refused at
   the apply (a dead, changed or reused actor or target), refused at the enqueue for a full queue, dropped as
   malformed (restored bytes enqueue never writes), refused at the enqueue for an actor with no incarnation, and
   refused for a verb other than NEW or QUEUE; the most entries pending after an enqueue. Diagnostics: no decision
   reads them. */
typedef struct IMPAI_REACT_COUNTS {
    uint32_t given, late, refused, full, bad, unborn, verb;
    uint32_t pend_max;
} IMPAI_REACT_COUNTS;

/* THE VIEW: the decision's WORLDQ with `order` replaced by the enqueue (exec_order), every other op the inner
   WORLDQ's own. It works by the view contract (worldq.h, struct WORLDQ): q.self and q.seat are the inner's, so
   each op answers through the view exactly as through the inner. A stack object of the turn, for one decision. */
typedef struct IMPAI_EXEC_VIEW {
    WORLDQ              q;          /* FIRST MEMBER: exec_order finds the view as (IMPAI_EXEC_VIEW*)q         */
    WORLDQ_OPS          ops;        /* a copy of the inner table, `order` replaced by exec_order              */
    const WORLDQ*       inner;
    IMPAI_EXEC*         x;
    uint32_t            due;        /* the decision's GameTime + react                                        */
    uint8_t             src;        /* IMPAI_SRC_ECO or IMPAI_SRC_MIL                                         */
    IMPAI_REACT_COUNTS* n;          /* the glue's diagnostics, or NULL                                        */
} IMPAI_EXEC_VIEW;

/* The WORLDQ a decision of manager `src` at GameTime `now` orders through: `inner` itself when the persona has no
   reaction (react 0, as at Hard: today's path, no view at all), else v's view, whose orders wait in x. */
const WORLDQ* impai_exec_view(IMPAI_EXEC_VIEW* v, const WORLDQ* inner, IMPAI_EXEC* x, const IMPAI_PERSONA* p,
                              uint32_t now, int src, IMPAI_REACT_COUNTS* n);

/* THE APPLY, every turn tick after the observation and before either decision: each entry due at `now` or before
   is given through q's order (q NULL: the seat's WORLDQ would not open, and each due entry is refused), each due
   later within the reaction kept, in its order, and any other dropped as malformed. The kept entries are
   compacted to the front and the rest of the queue zeroed. */
void impai_exec_apply(IMPAI_EXEC* x, const WORLDQ* q, const IMPAI_PERSONA* p, uint32_t now, IMPAI_REACT_COUNTS* n);

/* THE BUDGET: manager src's bucket refilled to `now` and the whole actions it holds, or -1 when the persona gives
   the manager no rate (apm 0, as at Hard). An action is 1800 units, so a rate of apm actions a minute (1800
   ticks) refills exactly apm units a tick, held to burst actions. */
int  impai_exec_acts(IMPAI_EXEC* x, int src, const IMPAI_PERSONA* p, uint32_t now);
/* debits `spent` actions, at most what impai_exec_acts last answered, so the bucket stays at or above 0 */
void impai_exec_spend(IMPAI_EXEC* x, int src, const IMPAI_PERSONA* p, int spent);

/* x at a seat's start: no entry pending, each bucket full */
void impai_exec_reset(IMPAI_EXEC* x, const IMPAI_PERSONA* p);

#endif
