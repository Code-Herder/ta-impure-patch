/* impai_react.c -- how a level carries out ImpAI's decisions: the persona (impai.h, IMPAI_PERSONA) and its rows,
   the reaction's view, queue and apply (research/notes/impai/brain.md, The plan and the personas). Every level
   plays the same plan, which decides the same way at Easy as at Hard; a level is only how fast and how much of
   it is carried out.

   CREDIT. The persona model is Nanolathe's Modern AI (github.com/nanolathe-gg/nanolathe, MIT-licensed, Copyright
   (c) 2026 Daniel G. Taylor and Nanolathe contributors): the persona's parameters (internal/aikit/persona.go);
   the host's rule of one pending batch, given when due (host.go, Step); the executor's token bucket, its rule
   that a group order is one action, and its binding of a command to the instance it was issued for (cmd.go,
   refill, apply, actorOK and targetOK). No code is copied: the C here is written from the formulas.

   THE BOUND. Memory: npend <= IMPAI_PEND_MAX by construction -- the enqueue refuses at capacity, the apply walks
   min(npend, IMPAI_PEND_MAX) -- and every slot, type and count an entry holds passed the inner WORLDQ's
   order_test at its enqueue, which bounds them as `order` does. Content: an economy decision enqueues at most
   roots_max <= plan.roots <= IMPAI_ROOTS_MAX, every economy order site testing d->roots first (impai_brain.c,
   impai_give), and a military decision at most mil_roots <= IMPAI_MIL_ROOTS_MAX, order() testing it. One batch
   a manager: react <= min(look) - 15 (impai_persona_ok) puts a manager's batch due before its next decision, and
   the apply that precedes that decision gives it, so the queue holds at most one economy batch and one military
   batch and `full` is never reached.

   THE ORDERING. One react a seat, fixed for the seat's life -- set at its init and restored with its state -- so
   the due ticks fall in the decisions' order and the queue, first in first out, gives the entries in decision
   order; within a decision in the order they were emitted, the order today's immediate orders ran in. The apply
   runs before the decisions, in the recorder's attribution bracket (tagpu_impai.c, impai_turn). Nothing reads a
   clock: a due tick is a GameTime.

   THE LIFETIME. No pointer survives a turn. An entry holds slots and incarnations, and the WORLDQ re-derives the
   unit and the target inside the order call (tagpu_worldq.c, ORDERS OUT, on the game thread that owns the unit
   array), refusing a slot whose unit is not the incarnation the entry was decided for. So a dead, changed or
   recycled actor or target costs one refused entry, never a call on freed memory. A birth and not the slot
   reuse's hold (tagpu_patches.c, THE HOLD), which covers one or two ticks against a reaction of up to 135.

   NO CHEATING. The delay is on orders, never on what is seen: every decision observes the world at its own tick
   through the same ops as with no reaction. No decision reads the queue, only its bucket's actions, given to it,
   and the other manager's commander marker, the seat's own intent. The enqueue's answer is order_test's, what
   `order` would answer at the decision's tick, so a delayed seat learns no more and no less than a seat with no
   delay; the apply's answers are counted for the log and reach no decision. The births an entry keeps come
   through observe, the visibility rule's own channel (worldq.h), and no decision reads them.

   Engine-free and without statics: everything it reads is its arguments. */
#include <string.h>
#include "impai_react.h"
#include "tagpu_simreg.h"

/* The persona's rules and rows, the reaction's queue and its apply. OWNER: a live ImpAI seat's turn alone
   enqueues and applies, and it plays no puppet seat (tagpu_impai.c, impai_play), whose WORLDQ refuses every order
   in any case; on a puppet the recording restores IMPAI_MIL, the queue and the persona with it, and the recorder
   stays the only writer. */
SIM_FEATURE(impai_react, SIM_PUPPET_OWNER, "impai-eco impai-fight");

/* THE PERSONA ROWS, Easy, Normal and Hard: starting points, the bar's to move (research/notes/impai/brain.md, The
   plan and the personas). Easy's and Normal's rates sum to 25 and 60 actions a minute, Nanolathe's Easy and
   Medium. Hard's is no limit at all -- looks of 15 and 30 ticks, no reaction, no budget, no attention limit --
   so Hard is the plan exactly as it decides it, the bar's Hard evidence's game. */
const IMPAI_PERSONA IMPAI_PERSONAS[IMPAI_LVS] = {
    /* look_eco, look_mil, react, apm_eco, apm_mil, burst_eco, burst_mil, att_eco, att_mil */
    { 60, 90, 45, 10, 15, 3, 3, 4, 2, { 0, 0 } },
    { 30, 45, 15, 24, 36, 4, 6, 8, 3, { 0, 0 } },
    { 15, 30,  0,  0,  0, 0, 0, 0, 0, { 0, 0 } },
};

#define LOOK_MAX  150           /* ticks: a look at most, so the horizon and the bucket's refill stay bounded       */
#define APM_MAX   600           /* actions a minute at most, ten a second: it bounds the bucket's refill            */
#define BURST_MAX 60            /* a bucket's actions at most                                                       */

/* Each rule's reason:
   - A LOOK IS A MULTIPLE OF 15, 15..LOOK_MAX. A seat's economy then decides only on ticks of 7 seat (mod 15) and
     its military only on ticks of 7 seat + 8 (mod 15), so the two managers never share a tick and no two seats'
     economies share one, whatever the looks: what impai_brain_due and impai_mil_due rest on.
   - react <= min(look_eco, look_mil) - 15. The batch decided at t is due at t + react, at most t + look - 15, so
     it is given before the same manager's next decision (THE BOUND, below: at most one batch a manager pending),
     and every decision reads its previous batch after at least 15 ticks of engine time, the gap the 15-tick
     economy look gives today, so no decision meets an order the engine has not stepped. It also forces react 0
     wherever a look is 15.
   - A BUDGET is a rate of 1..APM_MAX with a burst of 1..BURST_MAX, or none at all (both 0): the bucket's
     arithmetic (impai_exec_acts) is bounded by them.
   - ATTENTION is 0, none, or within the tables it limits: IMPAI_IDLE_CAP builders, IMPAI_SQUADS operations.
   - The pad is 0, so a recorded persona has one form. */
int impai_persona_ok(const IMPAI_PERSONA* p)
{
    const unsigned look = p->look_eco < p->look_mil ? p->look_eco : p->look_mil;
    if (p->look_eco < 15 || p->look_eco > LOOK_MAX || p->look_eco % 15 ||
        p->look_mil < 15 || p->look_mil > LOOK_MAX || p->look_mil % 15)
        return 0;
    if (p->react > look - 15) return 0;
    if (p->apm_eco > APM_MAX || p->apm_mil > APM_MAX) return 0;
    if (p->apm_eco ? p->burst_eco < 1 || p->burst_eco > BURST_MAX : p->burst_eco != 0) return 0;
    if (p->apm_mil ? p->burst_mil < 1 || p->burst_mil > BURST_MAX : p->burst_mil != 0) return 0;
    if (p->att_eco > IMPAI_IDLE_CAP || p->att_mil > IMPAI_SQUADS) return 0;
    return !p->pad[0] && !p->pad[1];
}

/* ---- the reaction ---- */

/* The view's `order`, the enqueue: the immediate answer, and on WQ_OK the command kept in the queue until due. */
static int exec_order(const WORLDQ* q, const WQ_CMD* cmd)
{
    IMPAI_EXEC_VIEW* v = (IMPAI_EXEC_VIEW*)q;
    IMPAI_EXEC* x = v->x;
    const WORLDQ* in = v->inner;
    IMPAI_PEND* e;
    WQ_OBS o;
    int rc;
    /* ImpAI gives no other verb (impai_brain_test.c, the verbs), and the queue keeps no REMOVE's node */
    if (cmd->verb != WQ_CMD_NEW && cmd->verb != WQ_CMD_QUEUE) {
        if (v->n) v->n->verb++;
        return WQ_REFUSED;
    }
    /* the answer an immediate order would have had at this tick, so a decision that falls back on a refusal does
       so in the same decision as today */
    if ((rc = in->ops->order_test(in, cmd)) != WQ_OK) return rc;
    if (x->npend >= IMPAI_PEND_MAX) {
        if (v->n) v->n->full++;
        return WQ_REFUSED;
    }
    /* FAILS CLOSED: an entry never acts on an incarnation it was not decided for, so an actor the world gives no
       incarnation is refused now */
    if (in->ops->observe(in, cmd->unit, 0, &o) != WQ_SEEN_OWN || !o.birth_known) {
        if (v->n) v->n->unborn++;
        return WQ_REFUSED;
    }
    e = &x->pend[x->npend];
    memset(e, 0, sizeof *e);
    e->due = v->due;
    e->birth = o.birth;
    memcpy(e->pos, cmd->pos, sizeof e->pos);
    e->unit = cmd->unit;
    e->target = cmd->target;
    e->type = cmd->type;
    e->verb = cmd->verb;
    e->action = cmd->action;
    e->shift = cmd->shift;
    e->count = (uint8_t)(cmd->verb == WQ_CMD_QUEUE ? cmd->count : 0);
    e->src = v->src;
    e->inc = WQ_INC_UNIT;
    /* a target whose incarnation the seat knows is bound to it; one it does not is held to liveness alone, the
       immediate order's own rule. Every target a decision names is its own unit or one it sees now */
    if (cmd->target != WQ_NONE && in->ops->observe(in, cmd->target, 0, &o) && o.birth_known) {
        e->inc |= WQ_INC_TARGET;
        e->tbirth = o.birth;
    }
    x->npend++;
    if (v->n && x->npend > v->n->pend_max) v->n->pend_max = x->npend;
    return WQ_OK;
}

const WORLDQ* impai_exec_view(IMPAI_EXEC_VIEW* v, const WORLDQ* inner, IMPAI_EXEC* x, const IMPAI_PERSONA* p,
                              uint32_t now, int src, IMPAI_REACT_COUNTS* n)
{
    if (!p->react) return inner;
    memset(v, 0, sizeof *v);
    v->ops = *inner->ops;
    v->ops.order = exec_order;
    v->q.ops = &v->ops;
    v->q.self = inner->self;
    v->q.seat = inner->seat;
    v->inner = inner;
    v->x = x;
    v->due = now + p->react;
    v->src = (uint8_t)src;
    v->n = n;
    return &v->q;
}

/* an entry as enqueue writes it: a verb it takes, a count in its range for the verb, a manager */
static int pend_formed(const IMPAI_PEND* e)
{
    if (e->src >= IMPAI_SRCS) return 0;
    if (e->verb == WQ_CMD_NEW) return e->count == 0;
    return e->verb == WQ_CMD_QUEUE && e->count >= 1 && e->count <= WQ_QUEUE_MAX;
}

void impai_exec_apply(IMPAI_EXEC* x, const WORLDQ* q, const IMPAI_PERSONA* p, uint32_t now, IMPAI_REACT_COUNTS* n)
{
    const unsigned walk = x->npend < IMPAI_PEND_MAX ? x->npend : IMPAI_PEND_MAX;
    unsigned k, kept = 0;
    if (n && x->npend > IMPAI_PEND_MAX) n->bad += x->npend - IMPAI_PEND_MAX;
    for (k = 0; k < walk; k++) {
        const IMPAI_PEND* e = &x->pend[k];
        if (!pend_formed(e)) {
            if (n) n->bad++;
            continue;
        }
        if (e->due <= now) {
            WQ_CMD c;
            int rc = WQ_REFUSED;
            memset(&c, 0, sizeof c);
            c.verb = e->verb;
            c.action = e->action;
            c.shift = e->shift;
            c.inc = (uint8_t)(WQ_INC_UNIT | (e->inc & WQ_INC_TARGET));
            c.unit = e->unit;
            c.target = e->target;
            memcpy(c.pos, e->pos, sizeof c.pos);
            c.type = e->type;
            c.count = e->count;
            c.birth = e->birth;
            c.tbirth = e->tbirth;
            if (q) rc = q->ops->order(q, &c);
            if (n) {
                if (rc == WQ_OK) n->given++;
                else n->refused++;
                if (e->due < now) n->late++;
            }
        } else if (e->due - now <= p->react) {
            if (kept != k) x->pend[kept] = *e;
            kept++;
        } else if (n) {
            n->bad++;               /* due past the reaction: a malformed restored entry */
        }
    }
    x->npend = (uint16_t)kept;
    x->pad = 0;
    memset(&x->pend[kept], 0, (IMPAI_PEND_MAX - kept) * sizeof x->pend[0]);
}

void impai_exec_reset(IMPAI_EXEC* x, const IMPAI_PERSONA* p)
{
    (void)p;
    memset(x, 0, sizeof *x);
}
