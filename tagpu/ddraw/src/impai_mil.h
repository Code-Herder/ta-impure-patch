#ifndef IMPAI_MIL_H
#define IMPAI_MIL_H
/* impai_mil.h -- ImpAI's military manager: the seat's second decision, on its own ticks (impai_mil.c;
   research/notes/impai/brain.md, The military). Its state is IMPAI_MIL (impai.h); its input the same WORLDQ
   view and knowledge the economy reads, the type information, the fight predictor's content and
   coefficients (impai_fight.h), the plan and the seat's persona. Engine-free: no engine header, no statics, no
   clock, no draw of the simulation's stream, no noise. */
#include "impai.h"
#include "impai_fight.h"

#define IMPAI_ARMY_MAX     512      /* army units a census lists; more are left out of every squad          */
#define IMPAI_ENEMY_MAX    2048     /* enemy rows the picture lists                                         */
#define IMPAI_PREDICT_CAP  16       /* the most predictions the plan may ask a decision for                 */
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
    const IMPAI_BRAIN*    brain;        /* the economy's state: the base anchor and the kept ways out (on_yard) */
    const IMPAI_PLAN*     plan;
    const IMPAI_PERSONA*  persona;      /* the seat's, validated by the caller (impai_persona_ok)             */
    int                   acts;         /* the military bucket's actions (impai_exec_acts); -1 no budget      */
    const IMPAI_REACH*    reach;        /* NULL: no reach test, and the judgement is 0                       */
    void*                 scratch; size_t scratch_bytes;   /* impai_mil_scratch_bytes(), the caller's         */
} IMPAI_MIL_IN;

/* What one decision did, against its bounds. */
typedef struct IMPAI_MIL_OUT {
    uint16_t army, enemies, contacts, threats;    /* listed; base threats among the contacts                 */
    uint16_t predictions, refused_predictions;    /* the predictor's answers, and its refusals               */
    uint16_t roots, refused, left_out;            /* orders given; resolver answers of 0 or -1; units past a
                                                     list's bound, the sum of the four below                 */
    uint16_t left_squad;                          /* army units no squad took: 0 by construction (become)    */
    uint16_t left_army, left_enemy, left_contact; /* past IMPAI_ARMY_MAX, IMPAI_ENEMY_MAX, IMPAI_CONTACTS    */
    uint8_t  tasks[IMPAI_SQ_TASKS];               /* squads by task after the decision                       */
    uint8_t  attacks, raids, retreats, scouts, defends, safe;   /* tasks begun in the decision               */
    uint8_t  waves[TASIM_LAYERS];                 /* the attacks begun, by layer (THE WAVE)                  */
    uint8_t  wave_resets;                         /* layers whose count a destroyed target reset (THE WAVE)  */
    uint8_t  dgun;                                /* 1 when the commander was ordered to D-gun               */
    uint8_t  leash;                               /* 1 when the leash moved the commander home               */
    uint16_t unreached;                           /* moves not given: no block of the member's component
                                                     near the point, or the substitutes' cache full          */
    uint16_t cross;                               /* predictions made with a side that cannot close          */
    uint8_t  press;                               /* 1 when the decision stamped THE PRESSURE                */
    uint16_t acts;                                /* actions spent: squads served and commander orders       */
    uint8_t  ops;                                 /* operations held at the decision's end (THE ATTENTION)   */
    uint16_t stalled;                             /* ATTACK goals given up (THE GOAL'S PROGRESS)             */
    uint8_t  lead, lead_was;                      /* THE LEAD after the decision, and before it              */
    float    own_v, enemy_v;                      /* the seat's live value, and the enemy's as it knows it   */
} IMPAI_MIL_OUT;

/* 1 when seat `seat` makes a military decision at `gametime`: GameTime % look == (7 seat + 8) % 15, its persona's
   look_mil. Every look is a multiple of 15 (impai_persona_ok), so the military's tick is 7 seat + 8 (mod 15) and
   the economy's 7 seat (mod 15) whatever the two looks: THE TWO NEVER SHARE A TICK, so a turn's cost is the
   larger of the two, not their sum, and ten seats spread over the fifteen phases as the economy's do. */
int    impai_mil_due(unsigned gametime, unsigned seat, unsigned look);
#define IMPAI_MIL_SCRATCH_MAX (768u * 1024u)
size_t impai_mil_scratch_bytes(void);
/* One military decision (impai_mil.c, the comment over it): 0, or -1 when the input is refused (a table short
   of the world, the scratch short), with nothing written. */
int    impai_mil_decide(const IMPAI_MIL_IN* in, IMPAI_MIL_OUT* out);
/* THE BLAST'S APPROACH (impai_mil.c): 1 when the point (x, z) px lies nearer than the commander at (cx, cz) to a
   foe's commander's last-seen point and within its blast keep grown by its age, for IMPAI_DRY_T after the sighting */
int    impai_blast_nearer(const WORLDQ* q, const IMPAI_MIL* st, const IMPAI_VIEW* v, const IMPAI_TINFO* ti,
                          unsigned nti, const IMPAI_PERSONA* p, uint32_t now, int32_t cx, int32_t cz, int32_t x,
                          int32_t z);
/* The task's letter in the pulse line: f, F, D, R, A, B (retreat: back), S */
char   impai_mil_letter(unsigned task);

#endif
