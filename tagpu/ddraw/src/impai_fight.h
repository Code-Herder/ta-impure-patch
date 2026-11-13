#ifndef IMPAI_FIGHT_H
#define IMPAI_FIGHT_H
/* impai_fight -- ImpAI's fight predictor: two groups of unit types in, each side's remaining value share
   and the margin out (research/notes/impai/brain.md, The fight predictor).

   THE CONTENT is the engine's own numbers for every unit type and weapon of the level, built at the level
   init by tagpu_worldq_fight_in from the def array and the weapon records (tagpu_impai.c, THE FIGHT
   CONTENT). THE MODEL (impai_fight.c) is a stepped Lanchester-type fight over that content with the
   coefficients IMPAI_FIT, fitted to engine fights by tools/impai/predfit.py, which calls this same C through
   a native shared library (tools/impai/fightlib.c): one implementation for the DLL and the fit.

   Engine-free and Win32-free: no globals, no state, no clock and no draw; every array is the caller's.
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

/* a weapon's kind, from its flags +0x111, tested in this order: beamweapon, dropped, guidance or tracks
   or vlaunch, ballistic, else direct (impai_fight_kind) */
enum { IMPAI_FK_BEAM, IMPAI_FK_DIRECT, IMPAI_FK_BALLISTIC, IMPAI_FK_GUIDED, IMPAI_FK_DROPPED, IMPAI_FKINDS };

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
    uint8_t  kind, pad;              /* IMPAI_FK_*                                                         */
} IMPAI_FWEAPON;

typedef struct IMPAI_FDMG { uint16_t type, pad; int32_t dmg; } IMPAI_FDMG;   /* a [DAMAGE] row, 8 bytes */

typedef char impai_ftype_bytes[sizeof(IMPAI_FTYPE) == 40 ? 1 : -1];
typedef char impai_fweapon_bytes[sizeof(IMPAI_FWEAPON) == 36 ? 1 : -1];
typedef char impai_fdmg_bytes[sizeof(IMPAI_FDMG) == 8 ? 1 : -1];

/* The kind of a weapon whose flags are `flags` (+0x111). */
int impai_fight_kind(uint32_t flags);

#define IMPAI_FSIDE_TYPES 16          /* the types a side holds; more are merged (impai_fight_side)        */
#define IMPAI_FSTEP       30          /* ticks a step                                                      */
#define IMPAI_FSTEPS      60          /* steps at most: 1800 ticks                                         */

typedef struct IMPAI_FENTRY { uint16_t type, n; uint16_t hp_frac; } IMPAI_FENTRY;   /* hp_frac / 65535 */
typedef struct IMPAI_FSIDE { IMPAI_FENTRY e[IMPAI_FSIDE_TYPES]; uint8_t ne, closes; } IMPAI_FSIDE;

/* The fitted coefficients (src/impai_fitparams.c, impai_fitparams). The hit probability of a shot of kind k
   is sigma(hit[k][0] + hit[k][1] lead + hit[k][2] spread + hit[k][3] area), the features as
   impai_fight.c's pair rate defines them. */
enum { IMPAI_FH_BIAS, IMPAI_FH_LEAD, IMPAI_FH_SPREAD, IMPAI_FH_AREA, IMPAI_FHS };
typedef struct IMPAI_FIT {
    float hit[IMPAI_FKINDS][IMPAI_FHS];
    float close;                      /* closing speed over the slowest mobile type's maxvelocity          */
    float focus;                      /* 0 fire spread by count .. 1 all on the quickest kill              */
    float splash;                     /* extra units an area hit reaches, per overlap                      */
    float tau;                        /* the margin's spread: P(A wins) = sigma(m / tau)                   */
} IMPAI_FIT;
#define IMPAI_FIT_COEFFS (IMPAI_FKINDS * IMPAI_FHS + 4)

extern const IMPAI_FIT impai_fitparams;

typedef struct IMPAI_FCONTENT {
    const IMPAI_FTYPE* t; unsigned nt; const IMPAI_FWEAPON* w; unsigned nw; const IMPAI_FDMG* d; unsigned nd;
} IMPAI_FCONTENT;

typedef struct IMPAI_FRESULT { float sa, sb, m; uint16_t steps; } IMPAI_FRESULT;

/* What a run did, for the test: each side's units killed and the shots aimed at it, summed over the steps. */
typedef struct IMPAI_FSTATS { uint32_t killed[2]; float shots[2]; } IMPAI_FSTATS;

/* The fight of side a against side b, their centroids dist_px apart: 0, or -1 when an input is refused (a
   type past nt, a weapon index past nw, a damage span past nd, a type with no hp, a side with more than
   IMPAI_FSIDE_TYPES entries or with no value): nothing is promised then. Deterministic: no clock, no draw;
   every loop bounded by the constants above. impai_fight_run is the same with the run's counts in *st. */
int impai_fight_predict(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a,
                        const IMPAI_FSIDE* b, int32_t dist_px, IMPAI_FRESULT* out);
int impai_fight_run(const IMPAI_FCONTENT* c, const IMPAI_FIT* p, const IMPAI_FSIDE* a, const IMPAI_FSIDE* b,
                    int32_t dist_px, IMPAI_FRESULT* out, IMPAI_FSTATS* st);

/* P(A wins) for margin m: sigma(m / tau). */
float impai_fight_pwin(const IMPAI_FIT* p, float m);

/* The pair rate the model fires with: the HP a tick one unit of type i deals a unit of type j, and the
   longest range among the weapons that hurt it, 0 and 0 when none does. 0, or -1 for a refused input. */
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
