#ifndef TASIM_ROLES_H
#define TASIM_ROLES_H
/* tasim_roles -- a unit type's role, layer group, tech level and value, from its def's fields:
   the one implementation of the role rules, for the calibration runner's duel generator (through
   `tasim_run roles`) and for ImpAI in the DLL. The rules: research/notes/modules/tasim-calibration.md,
   *Roles*; no Python copies a rule, a threshold or a percentile.

   THE INPUT is the def's own values as the engine holds them after its FBI parser 0x42BF40 and
   its weapon parser 0x42E440, so both fillers are plain copies: the engine adapter's of the
   engine's memory, the file filler's (tools/tasim/tasim_run.c) of the definitions the install
   loader read through the key table (tools/tasim/tasim_load.c, tasim_keys.h).

   Pure: no engine memory, no Win32, no globals, no state of its own; every array is the caller's
   and the scratch space is the caller's (tasim_roles_work). Doubles are rounded once per
   operation, so an i686 build with SSE2 maths and a native build give the same answer. */
#include <stddef.h>
#include <stdint.h>

/* R, the energy a unit of metal is worth: value = metal + energy / R. The retail metal maker
   (ARMMAKR, CORMAKR: EnergyUse=60, MakesMetal=1, the cheapest maker by metal cost) converts 60
   energy into 1 metal. One named constant (owner's decision), read by tools/tasim/calibref.py;
   tasim_roles_maker_ratio reports a mod's own maker beside it. */
#define TASIM_ROLES_R 60.0

#define TASIM_ROLE_WEAPONS 10           /* weapon1..weapon10: the extra-weapons module's ten */
#define TASIM_ROLE_BADSLOTS 3           /* def +0x231, +0x235, +0x239: the three slots' bad-target
                                           categories, primary, secondary, special */

/* the weapon's flag word +0x111, bit by bit (the key inventory, engine/keys.md) */
#define TASIM_WF_BALLISTIC   (1u << 1)
#define TASIM_WF_BEAM        (1u << 3)
#define TASIM_WF_VLAUNCH     (1u << 4)
#define TASIM_WF_PARALYZER   (1u << 7)
#define TASIM_WF_DROPPED     (1u << 8)
#define TASIM_WF_GUIDANCE    (1u << 12)
#define TASIM_WF_TRACKS      (1u << 13)
#define TASIM_WF_WATER       (1u << 16)
#define TASIM_WF_TOAIR       (1u << 17)
#define TASIM_WF_SELFPROP    (1u << 20)
#define TASIM_WF_TWOPHASE    (1u << 24)
#define TASIM_WF_COMMANDFIRE (1u << 26)
#define TASIM_WF_STOCKPILE   (1u << 28)
#define TASIM_WF_INTERCEPTOR (1u << 30)
/* Impure's weapon key `nottoair` (tagpu_datakeys.c, TAGPU_WK_NOTTOAIR: the weapon refuses every flying
   target), carried in bit 31, which the engine's weapon parser never writes into +0x111 (engine/weapons.md,
   What the loader writes into the record) */
#define TASIM_WF_NOTTOAIR    (1u << 31)

/* the def's flag words: +0x241 and +0x245 */
#define TASIM_F241_BUILDER    (1u << 6)
#define TASIM_F241_CANFLY     (1u << 11)
#define TASIM_F241_CANHOVER   (1u << 12)
#define TASIM_F241_AMPHIBIOUS (1u << 21)
#define TASIM_F241_KAMIKAZE   (1u << 28)
#define TASIM_F241_ANTIWEAPONS (1u << 29)
#define TASIM_F245_CANMOVE    (1u << 7)
#define TASIM_F245_COMMANDER  (1u << 18)

typedef struct {
    int32_t  range;                     /* +0xDC                                                */
    float    reload_ticks;              /* reloadtime x 30                                      */
    int32_t  burst;                     /* >= 1 when used                                       */
    int32_t  damage_default;            /* +0xD4, [DAMAGE] default                              */
    float    damage_air;                /* the mean of [DAMAGE] over the mod's aircraft types
                                           (category VTOL), each its entry or the default       */
    float    energy_per_shot;           /* +0xC0                                                */
    float    metal_per_shot;            /* +0xC4                                                */
    uint32_t flags;                     /* +0x111, TASIM_WF_*                                   */
} TASIM_ROLE_WEAPON;

typedef struct {
    char     name[32];                  /* unitname, def +0x20                                  */
    uint32_t flags241, flags245;
    uint8_t  bmcode;                    /* +0x22F: 1 a mover, 0 a building, else neither        */
    float    cost_energy, cost_metal;   /* +0x186, +0x18A                                       */
    uint32_t maxdamage;                 /* +0x1FA                                               */
    uint16_t workertime;                /* +0x1FE                                               */
    int32_t  maxvelocity_fx;            /* maxvelocity x 65536                                  */
    uint16_t sight, radar, jammer;      /* +0x202, +0x204, radardistancejam                     */
    int16_t  minwaterdepth, maxwaterdepth;
    float    energyuse;                 /* +0x1C6                                               */
    uint8_t  makesmetal;                /* +0x22D                                               */
    uint8_t  transportcapacity;
    uint8_t  slot_notair[TASIM_ROLE_BADSLOTS];   /* the slot's bad-target category is NOTAIR   */
    uint16_t nweapons;
    TASIM_ROLE_WEAPON w[TASIM_ROLE_WEAPONS];
    uint16_t explode_aoe;               /* the ExplodeAs weapon's areaofeffect (+0x220's record,
                                           its +0xD6), px; 0 none                               */
    uint16_t nbuild;                   /* build options: indices into the input array          */
    const uint16_t* build;
} TASIM_ROLE_IN;

typedef enum { TASIM_LAYER_LAND, TASIM_LAYER_SEA, TASIM_LAYER_AIR, TASIM_LAYERS } TASIM_LAYER;

typedef enum {
    TASIM_ROLE_BUILDER, TASIM_ROLE_DEFENCE, TASIM_ROLE_OTHER, TASIM_ROLE_ANTIAIR,
    TASIM_ROLE_SCOUT, TASIM_ROLE_ARTILLERY, TASIM_ROLE_RAIDER, TASIM_ROLE_ASSAULT, TASIM_ROLES
} TASIM_ROLE;

/* role flags */
#define TASIM_RF_COMMANDER   (1u << 0)
#define TASIM_RF_FACTORY     (1u << 1)
#define TASIM_RF_TRANSPORT   (1u << 2)
#define TASIM_RF_KAMIKAZE    (1u << 3)
#define TASIM_RF_ANTIWEAPONS (1u << 4)
#define TASIM_RF_RADAR       (1u << 5)
#define TASIM_RF_JAMMER      (1u << 6)
#define TASIM_RF_MOBILE      (1u << 7)

typedef struct {
    TASIM_ROLE  role;
    TASIM_LAYER layer;
    int         tech;                   /* 0: no build chain from a commander reaches it        */
    double      value;                  /* metal + energy / R                                   */
    double      dps_ground, dps_air;    /* per tick                                             */
    double      dps_water;              /* per tick, of the regular waterweapons (TASIM_WF_WATER)
                                           apart from the anti-air ones; also in dps_ground     */
    int32_t     range_ground;
    double      speed;                  /* world units per tick                                 */
    uint32_t    flags;                  /* TASIM_RF_*                                           */
} TASIM_ROLE_OUT;

/* The thresholds, per layer group: percentiles over its mobile non-builder units. */
typedef struct {
    double speed50[TASIM_LAYERS], speed75[TASIM_LAYERS];
    double value10[TASIM_LAYERS], range75[TASIM_LAYERS];
    int    population[TASIM_LAYERS];
} TASIM_ROLE_Q;

/* The scratch space classify needs for n units, in bytes. */
size_t tasim_roles_work(int n);

/* Every unit of `in` classified into `out`. `work` is tasim_roles_work(n) bytes of the caller's.
   Returns 0, or -1 when no unit has the commander bit (no tech level is defined), or -2 when a
   build option names an index past n. */
int tasim_roles_classify(const TASIM_ROLE_IN* in, int n, TASIM_ROLE_OUT* out, TASIM_ROLE_Q* q,
                         void* work);

/* Slot k (0 = weapon1) of unit u is anti-air: the classifier's own rule, which ImpAI's fight
   content applies too (impai_fight.h, IMPAI_FTYPE.air), so both call this one function. */
int tasim_roles_slot_antiair(const TASIM_ROLE_IN* u, int k);

/* energyuse / makesmetal of the metal maker with the lowest cost_metal (ties: the lower ratio);
   0 when the input holds none. */
double tasim_roles_maker_ratio(const TASIM_ROLE_IN* in, int n);

/* numpy.quantile's "linear" percentile of `x[0..n)`, which it sorts in place; 0 for n = 0. */
double tasim_roles_quantile(double* x, int n, double q);

const char* tasim_roles_name(TASIM_ROLE r);
const char* tasim_roles_layer_name(TASIM_LAYER l);

#endif
