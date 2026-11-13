#ifndef IMPAI_MIL_H
#define IMPAI_MIL_H
/* impai_mil.h -- ImpAI's military manager: the seat's second decision, on its own ticks (impai_mil.c;
   research/notes/impai/brain.md, The military). Its state is IMPAI_MIL (impai.h); its input the same WORLDQ
   view and knowledge the economy reads, the type information, the fight predictor's content and
   coefficients (impai_fight.h) and the level row. Engine-free: no engine header, no statics, no clock, no
   draw of the simulation's stream -- its only noise is impai_noise of the recorded seed. */
#include "impai.h"
#include "impai_fight.h"

#define IMPAI_ARMY_MAX     512      /* army units a census lists; more are left out of every squad          */
#define IMPAI_ENEMY_MAX    2048     /* enemy rows the picture lists                                         */
#define IMPAI_PREDICT_CAP  16       /* the most predictions any level row may ask a decision for            */
#define IMPAI_CONTACT_PX   768      /* contact_r: two seen enemies this close are one contact               */
#define IMPAI_CMD_SAFE_PX  600      /* cmd_safe_r: a contact this near the commander is weighed             */

/* What a military decision reads. */
typedef struct IMPAI_MIL_IN {
    const WORLDQ*         q;            /* the seat's instance, WQ_SITE_SEAT in the engine                    */
    const IMPAI_VIEW*     view;         /* its column of the knowledge rows, its allies, the threat table     */
    IMPAI_MIL*            st;
    IMPAI_JOB*            job; unsigned njob;     /* slot-indexed, njob >= nslots, shared with the economy    */
    const IMPAI_TINFO*    ti;  unsigned nti;      /* type-indexed, nti == ndefs                               */
    const IMPAI_FCONTENT* fc;           /* the fight content; no type (nt 0): every prediction is refused     */
    const IMPAI_FIT*      fit;
    const IMPAI_BRAIN*    brain;        /* the economy's state: the base anchor                               */
    const IMPAI_LEVEL*    lv;
    void*                 scratch; size_t scratch_bytes;   /* impai_mil_scratch_bytes(), the caller's         */
} IMPAI_MIL_IN;

/* What one decision did, against its bounds. */
typedef struct IMPAI_MIL_OUT {
    uint16_t army, enemies, contacts, threats;    /* listed; base threats among the contacts                 */
    uint16_t predictions, refused_predictions;    /* the predictor's answers, and its refusals               */
    uint16_t roots, refused, left_out;            /* orders given; resolver answers of 0 or -1; units past a
                                                     list's bound                                            */
    uint8_t  tasks[IMPAI_SQ_TASKS];               /* squads by task after the decision                       */
    uint8_t  attacks, raids, retreats, scouts, defends, safe;   /* tasks begun in the decision               */
    uint8_t  dgun;                                /* 1 when the commander was ordered to D-gun               */
} IMPAI_MIL_OUT;

/* 1 when seat `seat` makes a military decision at `gametime`: GameTime % mil_period == (7 seat + 8) % 15. Every
   mil_period is a multiple of 15, so the military's tick is 7 seat + 8 (mod 15) and the economy's 7 seat (mod
   15): THE TWO NEVER SHARE A TICK, so a turn's cost is the larger of the two, not their sum, and ten seats
   spread over the fifteen phases as the economy's do. */
int    impai_mil_due(unsigned gametime, unsigned seat, const IMPAI_LEVEL* lv);
#define IMPAI_MIL_SCRATCH_MAX (768u * 1024u)
size_t impai_mil_scratch_bytes(void);
/* One military decision (impai_mil.c, the comment over it): 0, or -1 when the input is refused (a table short
   of the world, the scratch short), with nothing written. */
int    impai_mil_decide(const IMPAI_MIL_IN* in, IMPAI_MIL_OUT* out);
/* The task's letter in the pulse line: f, F, D, R, A, B (retreat: back), S */
char   impai_mil_letter(unsigned task);

#endif
