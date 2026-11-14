#ifndef IMPAI_FIGHT_H
#define IMPAI_FIGHT_H
/* impai_fight -- ImpAI's fight predictor: two groups of unit types in, each side's remaining value share
   and the margin out (research/notes/impai/brain.md, The fight predictor).

   THE CONTENT is the engine's own numbers for every unit type and weapon of the level, built at the level
   init by tagpu_worldq_fight_in from the def array and the weapon records (tagpu_impai.c, THE FIGHT
   CONTENT). THE MODEL (impai_fight.c) is a stepped fight of volleys over that content with the
   coefficients IMPAI_FIT, fitted to engine fights by tools/impai/predfit.py, which calls this same C through
   a native shared library (tools/impai/fightlib.c): one implementation for the DLL and the fit.

   Engine-free and Win32-free: no globals, no state, no clock and no draw; every array is the caller's, the
   run's working state (IMPAI_FWORK) included.
   Single-precision floats rounded once per operation on SSE2 in every build (the Makefile's IMPAI rule),
   and no libm transcendental (impai_expf), so the DLL, the fit's library and the test's three builds give
   one answer bit for bit. Every struct holds only fixed-width fields at their natural alignment, so its
   bytes are the same on i686 and x86-64: the fit reads the tables' bytes out of the running engine and
   hands them to the library as they are. */
#include <stdint.h>
#include "tasim_roles.h"

#define IMPAI_FW_SLOTS   TASIM_ROLE_WEAPONS  /* weapon1..weapon10: the extra-weapons module's count     */
#define IMPAI_FWEAPONS   4096                /* the raised weapon table's slots (TAGPU_LIM_WEAPONS)      */
#define IMPAI_FDMG_POOL  65536               /* [DAMAGE] rows resolved to types, in all                  */

/* A weapon's kind, from its flags +0x111, by what the engine's projectile tick 0x49B720 does with its shot,
   tested in this order (impai_fight_kind):
     DROPPED   `dropped` (bit 8);
     HOMING    the shot follows a moving unit: selfprop (bit 20) and either `guidance` (bit 12) without
               twophase (bit 24), or twophase with `tracks` (bit 13). The tick steers a selfprop shot through
               the guidance 0x49B3E0 and the steer 0x49B520 (0x49BA5D..0x49BA64) when it is twophase and its
               phase bits +0x69 & 0x30 are set, or when it is not twophase and has `guidance`
               (0x49BA34..0x49BA53); at a twophase shot's phase change a shot without `tracks` (`test ah,0x20`,
               0x49BB2F) has its target unit cleared (0x49BB34..0x49BB37), so it steers to a point. A selfprop
               shot lives (range << 16) / velocity ticks, else def +0xE6 (0x49CB1C..0x49CB65). DISASSEMBLED.
               So torpedoes are homing; ARMMH's vlaunch twophase missile without `tracks` is not;
     BALLISTIC `ballistic` (bit 1);
     STRAIGHT  everything else: beams, direct guns, unguided rockets and missiles guided to a point. */
enum { IMPAI_FK_STRAIGHT, IMPAI_FK_BALLISTIC, IMPAI_FK_HOMING, IMPAI_FK_DROPPED, IMPAI_FKINDS };

#define IMPAI_FT_MOBILE    0x01u   /* canmove and not a building: TASIM_RF_MOBILE                          */
#define IMPAI_FT_FLIES     0x02u   /* canfly: only an anti-air slot hurts it                               */
#define IMPAI_FT_SUBMERGED 0x04u   /* afloat, its top is below the sea: only a water weapon hurts it      */
#define IMPAI_FT_COMMANDER 0x08u

typedef struct IMPAI_FTYPE {         /* one unit type, its def index the row, 40 bytes                    */
    uint32_t hp;                     /* maxdamage, def +0x1FA                                              */
    float    value;                  /* metal + energy / TASIM_ROLES_R, the classifier's value            */
    int32_t  speed;                  /* maxvelocity, def +0x192, 16.16 px a tick                           */
    uint16_t w[IMPAI_FW_SLOTS];      /* slot k's weapon record index, 0 for none or a weapon left out      */
    uint8_t  layer, fx, fz, flags;   /* TASIM_LAYER; footprint in cells, def +0x14A/+0x14C held to 0..255;
                                        IMPAI_FT_*                                                         */
    uint16_t air;                    /* bit k: slot k is anti-air (tasim_roles_slot_antiair): the slot
                                        hurts aircraft. A unit's slot, not its weapon's: the bad-target
                                        category that makes a slot anti-air is the def's                   */
    uint8_t  role;                   /* TASIM_ROLE, for the merge of a side past IMPAI_FSIDE_TYPES         */
    uint8_t  klass;                  /* impai_class_of (impai_table.h), IMPAI_K_NONE for no army class     */
} IMPAI_FTYPE;

typedef struct IMPAI_FWEAPON {       /* one weapon record, its index the row, 36 bytes                     */
    int32_t  range;                  /* +0xDC, px                                                          */
    int32_t  velocity;               /* +0x68, 16.16 px a tick; 0 for an instant weapon                    */
    uint16_t reload, burst;          /* +0xE4 ticks, +0xEA                                                 */
    uint16_t dmg, aoe;               /* +0xD4 the [DAMAGE] default, +0xD6 px                               */
    float    edge;                   /* +0xD8 edgeeffectiveness                                            */
    uint16_t accuracy, spray;        /* +0x104, +0xEE, in 1/65536 of a turn                                */
    uint32_t flags;                  /* +0x111 as TASIM_WF_*, bit 31 Impure's nottoair                    */
    uint32_t d0;                     /* its resolved [DAMAGE] rows: d[d0 .. d0 + nd), sorted by type        */
    uint16_t nd;
    uint8_t  kind, pad;              /* IMPAI_FK_*: impai_fight_kind(flags), checked against IMPAI_FKINDS  */
} IMPAI_FWEAPON;

typedef struct IMPAI_FDMG { uint16_t type, pad; int32_t dmg; } IMPAI_FDMG;   /* a [DAMAGE] row, 8 bytes */

typedef char impai_ftype_bytes[sizeof(IMPAI_FTYPE) == 40 ? 1 : -1];
typedef char impai_fweapon_bytes[sizeof(IMPAI_FWEAPON) == 36 ? 1 : -1];
typedef char impai_fdmg_bytes[sizeof(IMPAI_FDMG) == 8 ? 1 : -1];

/* The kind of a weapon whose flags are `flags` (+0x111). */
int impai_fight_kind(uint32_t flags);

#define IMPAI_FSIDE_TYPES 16          /* the types a side holds; more are merged (impai_fight_side)        */
#define IMPAI_FSIDE_WEAPONS (IMPAI_FSIDE_TYPES * IMPAI_FW_SLOTS)   /* every slot of every entry: a side is never
                                                                     refused for its weapon count           */
/* THE HORIZON is 5400 ticks, the references' limit (calib.limit): a quarter of the land reference fights and
   58 % of the naval ones last past 1800 ticks (MEASURED 2026-11-13 over 6725 reference runs), and a fight is
   scored where it stands at the limit, as the references score it. Its first 1800 ticks, where the openings
   and the first volleys fall, are in steps of 30; the rest in steps of 90, which gave the same check as 180
   steps of 30 for 56 % of the work. */
#define IMPAI_FSTEP0      30          /* ticks a step of the first part                                    */
#define IMPAI_FSTEPS0     60
#define IMPAI_FSTEP1      90          /* ticks a step of the second part                                   */
#define IMPAI_FSTEPS1     40
#define IMPAI_FSTEPS      (IMPAI_FSTEPS0 + IMPAI_FSTEPS1)   /* steps at most                                 */
#define IMPAI_FHORIZON    5400
_Static_assert(IMPAI_FSTEP0 * IMPAI_FSTEPS0 + IMPAI_FSTEP1 * IMPAI_FSTEPS1 == IMPAI_FHORIZON,
               "the two parts of the steps cover the horizon");
/* px a closer stands inside its first armed slot's range (impai_fight.c, chase_reach): Attack_Chase 0x4034A0
   stops the unit on the first tick can-engage passes (0x4035D6, the stop 0x4898B0 at 0x4035F2 and 0x4035FB)
   and it brakes inside. 32 is [INFERRED], fitted
   against 0, 16 and 48 (MEASURED 2026-11-13 on the 197 references then built); the braking distance of each
   type would replace it. */
#define IMPAI_FSTANDOFF   32

/* An entry of a side: its type, its live count, its health hp_frac / 65535, and IMPAI_FE_* flags. */
#define IMPAI_FE_NO_COMMANDFIRE 0x0001u   /* the entry fires no CommandFire weapon (TASIM_WF_COMMANDFIRE): an ImpAI
                                             commander whose D-gun the seat would not order now (impai_mil.c,
                                             THE STAND)                                                     */
typedef struct IMPAI_FENTRY { uint16_t type, n; uint16_t hp_frac, flags; } IMPAI_FENTRY;
typedef struct IMPAI_FSIDE { IMPAI_FENTRY e[IMPAI_FSIDE_TYPES]; uint8_t ne, closes; } IMPAI_FSIDE;
typedef char impai_fside_bytes[sizeof(IMPAI_FSIDE) == 130 ? 1 : -1];

/* The fitted coefficients (src/impai_fitparams.c, impai_fitparams). The hit probability of a shot of kind k
   on a target standing is sigma(hit[k][BIAS]), on one moving at its maxvelocity sigma(hit[k][BIAS] +
   hit[k][LEAD] x lead), lead as impai_fight.c defines it for the kind (for HOMING the pursuit term). */
enum { IMPAI_FH_BIAS, IMPAI_FH_LEAD, IMPAI_FHS };
typedef struct IMPAI_FIT {
    float hit[IMPAI_FKINDS][IMPAI_FHS];
    float focus;                      /* 0 fire spread by count .. 1 all on the quickest kill              */
    float splash;                     /* extra units an area hit reaches, per overlap                      */
    float tau;                        /* the margin's spread: P(A wins) = sigma(m / tau)                   */
} IMPAI_FIT;
#define IMPAI_FIT_COEFFS (IMPAI_FKINDS * IMPAI_FHS + 3)
typedef char impai_fit_bytes[sizeof(IMPAI_FIT) == 44 ? 1 : -1];

extern const IMPAI_FIT impai_fitparams;

typedef struct IMPAI_FCONTENT {
    const IMPAI_FTYPE* t; unsigned nt; const IMPAI_FWEAPON* w; unsigned nw; const IMPAI_FDMG* d; unsigned nd;
    /* the level's gravity, main+0x14263, an i32 16.16 px a tick squared, read on the game thread at the
       decision: it caps a ballistic weapon's reach (impai_fight.c). DATA until bounded: the predictor uses it
       only in 1..65536, the bound tagpu_rangeown.c applies, and treats anything else as 0, no cap. */
    int32_t gravity;
} IMPAI_FCONTENT;

/* One side's state in a run (impai_fight.c): per entry its type, live count, effective hp, the damage pooled
   and the shots aimed at it not yet spent on a kill, its travel toward the other side, the share of a step it
   moved, its speed and its stand-off against each enemy entry; per weapon entry its type entry, range, reload,
   burst, countdown, and on each enemy entry its damage a hit, its hit probabilities standing and moving, and
   its area's overlap. */
typedef struct IMPAI_FSTATE {
    unsigned ne, nw;
    uint16_t type[IMPAI_FSIDE_TYPES];
    int32_t  n[IMPAI_FSIDE_TYPES];
    float    hpe[IMPAI_FSIDE_TYPES], pool[IMPAI_FSIDE_TYPES], spool[IMPAI_FSIDE_TYPES];
    float    travel[IMPAI_FSIDE_TYPES], frac[IMPAI_FSIDE_TYPES], speed[IMPAI_FSIDE_TYPES];
    int32_t  stop[IMPAI_FSIDE_TYPES][IMPAI_FSIDE_TYPES];
    float    hv0;
    uint8_t  we[IMPAI_FSIDE_WEAPONS];
    int32_t  wrange[IMPAI_FSIDE_WEAPONS], wreload[IMPAI_FSIDE_WEAPONS], wburst[IMPAI_FSIDE_WEAPONS],
             wready[IMPAI_FSIDE_WEAPONS];
    float    dmg[IMPAI_FSIDE_WEAPONS][IMPAI_FSIDE_TYPES], p0[IMPAI_FSIDE_WEAPONS][IMPAI_FSIDE_TYPES],
             p1[IMPAI_FSIDE_WEAPONS][IMPAI_FSIDE_TYPES], ov[IMPAI_FSIDE_WEAPONS][IMPAI_FSIDE_TYPES];
} IMPAI_FSTATE;

/* A run's working state, the caller's (about 90 KB): no state crosses calls, and a run writes every field it
   reads before it reads it. */
typedef struct IMPAI_FWORK { IMPAI_FSTATE a, b; } IMPAI_FWORK;

typedef struct IMPAI_FRESULT { float sa, sb, m; uint16_t steps; } IMPAI_FRESULT;

/* What a run did, for the test: each side's units killed and the shots aimed at it, summed over the steps. */
typedef struct IMPAI_FSTATS { uint32_t killed[2]; float shots[2]; } IMPAI_FSTATS;

/* The fight of side a against side b, their FRONTS gap_px apart -- the distance between the two sides'
   nearest unit centres, which the ranges are tested against (impai_fight.c, E1) -- in the caller's work w: 0,
   or -1 when an input is refused (a type past nt, a weapon index past nw, a weapon kind past IMPAI_FKINDS, a
   damage span past nd, a type with no hp, a side with more than IMPAI_FSIDE_TYPES entries or with no value):
   nothing is promised then. Deterministic: no clock, no draw; every loop bounded by the constants above.
   impai_fight_run is the same with the run's counts in *st. */
int impai_fight_predict(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a,
                        const IMPAI_FSIDE* b, int32_t gap_px, IMPAI_FWORK* w, IMPAI_FRESULT* out);
int impai_fight_run(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a, const IMPAI_FSIDE* b,
                    int32_t gap_px, IMPAI_FWORK* w, IMPAI_FRESULT* out, IMPAI_FSTATS* st);

/* P(A wins) for margin m: sigma(m / tau). */
float impai_fight_pwin(const IMPAI_FIT* p, float m);

/* The pair rate: the HP a tick one unit of type i at full health deals units of type j moving at their
   maxvelocity, its volleys spread over the reload, and the longest reach among the weapons that hurt j, 0 and
   0 when none does. 0, or -1 for a refused input. */
int impai_fight_rate(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, unsigned i, unsigned j, float* rate,
                     int32_t* range);

/* impai_fight_rate as the buy's IMPAI_PREDICTOR.rate (impai_table.h): ctx an IMPAI_FCTX, the answer the pair
   rate r_ij, 0 for a refused pair. Its IMPAI_PREDICTOR.expf is impai_expf. */
typedef struct IMPAI_FCTX { const IMPAI_FCONTENT* c; const IMPAI_FIT* p; } IMPAI_FCTX;
float impai_fight_rate_of(const void* ctx, uint16_t i, uint16_t j);

/* A side from a list of n entries, merged to IMPAI_FSIDE_TYPES (impai_fight.c, THE MERGE): 0, or -1 for a
   type past nt. */
int impai_fight_side(const IMPAI_FCONTENT* c, const IMPAI_FENTRY* in, unsigned n, int closes,
                     IMPAI_FSIDE* out);

/* exp(x) by a fixed polynomial, the same in every build (impai_fight.c) */
float impai_expf(float x);

#endif
