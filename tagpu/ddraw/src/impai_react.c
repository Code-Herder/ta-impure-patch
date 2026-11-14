/* impai_react.c -- how a level carries out ImpAI's decisions: the persona (impai.h, IMPAI_PERSONA) and its rows
   (research/notes/impai/brain.md, The plan and the personas). Every level plays the same plan, which decides the
   same way at Easy as at Hard; a level is only how fast and how much of it is carried out.

   CREDIT. The persona model is Nanolathe's Modern AI (github.com/nanolathe-gg/nanolathe, MIT-licensed, Copyright
   (c) 2026 Daniel G. Taylor and Nanolathe contributors): the persona's parameters (internal/aikit/persona.go).
   No code is copied: the C here is written from the formulas.

   Engine-free and without statics: everything it reads is its arguments. */
#include <string.h>
#include "impai.h"
#include "tagpu_simreg.h"

/* The persona's rules and rows. OWNER: a persona is read only by a live ImpAI seat's turn, which plays no
   puppet seat (tagpu_impai.c, impai_play); a puppet's IMPAI_MIL, its persona with it, comes from the recording. */
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
