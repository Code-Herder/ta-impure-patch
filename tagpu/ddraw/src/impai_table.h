#ifndef IMPAI_TABLE_H
#define IMPAI_TABLE_H
/* impai_table.h -- how ImpAI's army is bought: the thirteen classes a unit type falls into, the picture of the
   enemy a seat has seen, the response table that turns the picture into each class's share of the army, and the
   buy, which turns the shares into units. research/notes/impai/brain.md, Adaptation.

   ONE IMPLEMENTATION. The DLL's production and the tuning tools (tools/impai/tune.py's composition fights) buy
   through these functions, so the armies tier A fights are the armies ImpAI would build.

   Engine-free: no engine header, no Win32, no globals but the two const tables, no clock, no draw. Every float
   is rounded once per operation on SSE2 (the Makefile's rule for this file), so the DLL, the native tools and
   the test's three builds (tests/impai_table_test.py) buy the same army. The type choice's exponential and
   the predictor's pair rate come from the caller (IMPAI_PREDICTOR), never from libm. */
#include <stddef.h>
#include <stdint.h>
#include "tasim_roles.h"

/* ---- the classes ---- */

/* A class is a role on a layer, with the air layer split by weapon kind:
     land: raider, assault, artillery, anti-air, scout -- the role classifier's roles (tasim_roles.h);
     sea:  raider, assault, artillery, anti-air -- a sea scout is no class;
     air:  fighter (the classifier's anti-air), bomber (a DROPPED weapon), air scout (the classifier's scout),
           gunship (any other armed flyer), tested in that order.
   Builders, the commander, defences, transports, kamikaze, stockpile units and everything the classifier calls
   OTHER are no class: the army does not buy them, and defences stay the economy's. Derived from the classifier's
   output and the def's weapons alone, so every mod is classified by the same rule (one table for every mod,
   research/notes/impai/decisions.md). */
enum { IMPAI_K_LRAIDER, IMPAI_K_LASSAULT, IMPAI_K_LARTY, IMPAI_K_LAA, IMPAI_K_LSCOUT,
       IMPAI_K_SRAIDER, IMPAI_K_SASSAULT, IMPAI_K_SARTY, IMPAI_K_SAA,
       IMPAI_K_FIGHTER, IMPAI_K_BOMBER, IMPAI_K_GUNSHIP, IMPAI_K_ASCOUT, IMPAI_KS };
#define IMPAI_K_NONE 255

#define IMPAI_K_AIRMASK ((1u << IMPAI_K_FIGHTER) | (1u << IMPAI_K_BOMBER) | (1u << IMPAI_K_GUNSHIP) | \
                         (1u << IMPAI_K_ASCOUT))
#define IMPAI_K_AAMASK  ((1u << IMPAI_K_LAA) | (1u << IMPAI_K_SAA))     /* the classes aa_cap bounds        */

/* The weapon flag +0x111 bit 3, `beamweapon` (research/notes/engine/keys.md): a weapon with it is a beam even
   when it also carries `dropped`, as the fight content's kind test orders them (beam before dropped). */
#define IMPAI_WF_BEAMWEAPON (1u << 3)

/* The class of the type whose classifier input is `in` and output `out`; IMPAI_K_NONE for none. */
unsigned impai_class_of(const TASIM_ROLE_IN* in, const TASIM_ROLE_OUT* out);
const char* impai_class_name(unsigned k);

/* ---- the picture ---- */

/* The twelve features a table reads, each in [0, 1]. The first nine are shares of the seat's seen enemy value:
   land raiders, land assault, land artillery, every sea class, fighters, bombers, gunships, ground defences, and
   anti-air (the land anti-air class and anti-air structures). Then the seat's income against the enemy's
   estimate (the seen extractors' count ratio, capped at 2, halved), the game minute over 40, the seat's highest
   factory tech over 3.
   Two more are derived from them for the response rows, never stored: AIR, the three armed air shares summed,
   and LOWAA, the defences anti-air does not cover, max(0, DEFENCE - AA). */
enum { IMPAI_F_LRAIDER, IMPAI_F_LASSAULT, IMPAI_F_LARTY, IMPAI_F_SEA, IMPAI_F_FIGHTER, IMPAI_F_BOMBER,
       IMPAI_F_GUNSHIP, IMPAI_F_DEFENCE, IMPAI_F_AA, IMPAI_F_INCOME, IMPAI_F_TIME, IMPAI_F_TECH,
       IMPAI_FEATURES,
       IMPAI_F_AIR = IMPAI_FEATURES, IMPAI_F_LOWAA, IMPAI_FROWS };

#define IMPAI_PIC_ENEMY 16              /* enemy mobile types the type choice weighs: a menu's types at most  */

/* One enemy mobile type and its share of the seen enemy mobile value. */
typedef struct IMPAI_ENEMY { uint16_t type, pad; float share; } IMPAI_ENEMY;

typedef struct IMPAI_PICTURE {
    float       f[IMPAI_FEATURES];
    float       minute;                 /* the game minute: GameTime / 1800                                   */
    uint32_t    ne;                     /* e[0 .. ne), at most IMPAI_PIC_ENEMY                                */
    IMPAI_ENEMY e[IMPAI_PIC_ENEMY];
} IMPAI_PICTURE;

/* What the features are counted from: the value the seat has seen of each enemy class, of enemy ground defences
   and anti-air structures, and of every enemy unit it counts (`total`, the denominator of every share); the
   extractors it holds and has seen the enemy hold; GameTime; its highest factory tech level. */
typedef struct IMPAI_PICTURE_IN {
    float    kval[IMPAI_KS];
    float    defence, aa_struct, total;
    uint32_t own_extractors, enemy_extractors;
    uint32_t gametime;
    uint32_t tech;
} IMPAI_PICTURE_IN;

/* p->f and p->minute from `in`; p->ne and p->e are the caller's. A share with total <= 0 is 0; the income with
   no enemy extractor seen is 1 when the seat holds one and 0.5 when neither does. */
void impai_picture_features(const IMPAI_PICTURE_IN* in, IMPAI_PICTURE* p);

/* ---- the table ---- */

/* THE TABLE'S SHAPE IS THE DESIGN, THE NUMBERS ARE TUNED. A class's share is
       s_k = clamp(base_k + sum over rows naming k of gain x feature, 0, 1)
   with the classes the seat cannot build at 0 and the air classes at 0 before `air_from_t`; then the anti-air
   classes together take min(their sum, aa_cap) of the army and every other class the rest in proportion
   (impai_table_shares). The rows' (feature, class) pairs are fixed by name in the default table and tuning
   moves only the gains: a free class-by-class matrix (13 x 14 numbers) could not be tuned in the engine in
   hours, this table's 9 tier-A numbers can (research/notes/impai/brain.md, Adaptation).

   A row's class is one class, or a group whose members the gain is shared among equally, counting only the
   members the seat can build, so the group as a whole moves by gain x feature: AA, the two anti-air classes,
   and SEA, the four sea classes. A row's feature is one of the twelve or the two derived (IMPAI_F_AIR,
   IMPAI_F_LOWAA).

   THE FILE. A table on disk is these 132 bytes, little-endian as they lie in memory, followed by their FNV-1a
   64 as 8 little-endian bytes (tools/impai/tablefile.py writes it). impai_table_load refuses the whole file on a
   size or hash mismatch, a row naming a feature or class past its count, a non-zero pad or a non-finite
   number, and clamps each base share and aa_cap to [0, 1]: a loaded table is data that has been bounded. */
#define IMPAI_RESPONSES 8
enum { IMPAI_KG_AA = IMPAI_KS, IMPAI_KG_SEA, IMPAI_KROWS };

typedef struct IMPAI_RESPONSE { uint8_t feature, klass; uint16_t pad; float gain; } IMPAI_RESPONSE;

typedef struct IMPAI_TABLE {
    float          base[IMPAI_KS];      /* a class's share before the picture                               */
    IMPAI_RESPONSE r[IMPAI_RESPONSES];  /* share[klass] += gain x feature                                    */
    float          counter;             /* the type choice's temperature (impai_class_type)                  */
    float          air_from_t;          /* the game minute before which no air class is bought               */
    float          aa_cap;              /* the most of the army the anti-air classes may take                */
    float          tech_m;              /* metal income a pass before tech-up                                */
} IMPAI_TABLE;

#define IMPAI_TABLE_BYTES      132u
#define IMPAI_TABLE_FILE_BYTES (IMPAI_TABLE_BYTES + 8u)

/* The response rows by name: the slot each pair holds in every table. */
enum { IMPAI_R_AA_AIR,              /* anti-air classes against seen air                                      */
       IMPAI_R_LARTY_DEFENCE,       /* artillery against seen defences                                        */
       IMPAI_R_LRAIDER_LARTY,       /* raiders against seen artillery                                         */
       IMPAI_R_LASSAULT_LRAIDER,    /* assault against seen raiders                                           */
       IMPAI_R_FIGHTER_BOMBER,      /* fighters against seen bombers                                          */
       IMPAI_R_FIGHTER_GUNSHIP,     /* fighters against seen gunships                                         */
       IMPAI_R_BOMBER_LOWAA,        /* bombers against defences the anti-air does not cover                   */
       IMPAI_R_SEA_SEA };           /* sea classes against seen sea                                           */

/* A2's hand-written composition (raider 0.25, assault 0.45, artillery 0.15, scout 0.05, the anti-air response
   min(0.35, 1.5 x seen air share)) as a table, on both surface layers and with no aircraft. */
extern const IMPAI_TABLE IMPAI_TABLE_HAND;
/* The table a seat plays unless the tournament lever names a file; IMPAI_TABLE_HAND's numbers until a tuning
   run emits its own (with the command and the table hash that produced it, in impai_table.c). */
extern const IMPAI_TABLE IMPAI_TABLE_DEFAULT;

enum { IMPAI_TABLE_OK = 0, IMPAI_TABLE_ESIZE = -1, IMPAI_TABLE_EHASH = -2, IMPAI_TABLE_EROW = -3,
       IMPAI_TABLE_EPAD = -4, IMPAI_TABLE_ENAN = -5 };

/* FNV-1a 64 over the table's 132 bytes. */
uint64_t impai_table_hash(const IMPAI_TABLE* t);
/* The file's bytes into *out, bounded as above; *hash the file's trailer. IMPAI_TABLE_OK, or the first
   refusal, and then *out is untouched. */
int impai_table_load(const void* bytes, size_t n, IMPAI_TABLE* out, uint64_t* hash);
/* `t`'s 140 file bytes into out[]. */
void impai_table_file(const IMPAI_TABLE* t, uint8_t out[IMPAI_TABLE_FILE_BYTES]);
const char* impai_table_error(int code);

/* Each class's share of the army, s[0 .. IMPAI_KS) summing to 1 or all 0 (nothing buildable), for a seat
   that can build the classes of `buildable` (bit k: class k). */
void impai_table_shares(const IMPAI_TABLE* t, const IMPAI_PICTURE* p, unsigned buildable, float s[IMPAI_KS]);

/* ---- the buy ---- */

/* What the type choice asks of the fight predictor: `rate(ctx, i, j)`, the damage one unit of type i deals to
   units of type j, HP a tick (the predictor's pair rate r_ij), and `expf`, the predictor's own exponential;
   both deterministic and bit-identical in every build. A NULL predictor, or one without both functions, scores
   every type 0 and the choice buys the cheapest. */
typedef struct IMPAI_PREDICTOR {
    float (*rate)(const void* ctx, uint16_t i, uint16_t j);
    float (*expf)(float x);
    const void* ctx;
} IMPAI_PREDICTOR;

/* One army type the seat can build: its type, its class and its value (metal + energy / TASIM_ROLES_R). */
typedef struct IMPAI_MENU_TYPE { uint16_t type; uint8_t klass, pad; float value; } IMPAI_MENU_TYPE;
#define IMPAI_MENU_MAX 256              /* types a menu holds: more are refused                              */
typedef struct IMPAI_MENU { const IMPAI_MENU_TYPE* t; uint32_t n; } IMPAI_MENU;

/* The classes `m` holds, bit k for class k. */
unsigned impai_menu_classes(const IMPAI_MENU* m);

/* The type of class k the seat queues: of m's types of class k with a positive finite value, the one of highest
       score_t = expf(counter x e_t) / value_t
   with e_t the type's rate against the picture's enemy composition, sum over e of share_e x rate(t, e.type),
   divided by the class's highest such rate (0 when it is not positive), so e_t is in [0, 1] and counter = 0
   buys the cheapest. Ties go to the lower type number. -1 when m holds no such type of class k. */
int impai_class_type(const IMPAI_TABLE* t, const IMPAI_PICTURE* p, const IMPAI_MENU* m, unsigned k,
                     const IMPAI_PREDICTOR* pred);

/* The class the next unit is bought in: of the classes in `eligible` with a share above 0 and a unit cost
   cost[k] > 0 that fits in `left`, the one furthest below its share of the army's value so far, the largest
   share[k] x sum(spent) - spent[k]; ties go to the lower class. -1 when none. The DLL's factory step asks it
   with its layer's classes and an unbounded `left`; impai_buy asks it with the budget's remainder. */
int impai_buy_next(const float share[IMPAI_KS], const float spent[IMPAI_KS], const float cost[IMPAI_KS],
                   unsigned eligible, float left);

#define IMPAI_BUY_UNITS 512             /* units one buy makes at most: the loop's bound                     */

typedef struct IMPAI_ARMY {
    float    share[IMPAI_KS];           /* impai_table_shares'                                               */
    int32_t  type[IMPAI_KS];            /* impai_class_type's, -1 for none                                   */
    uint16_t n[IMPAI_KS];               /* units bought of the class                                         */
    uint32_t units;
    float    spent;                     /* their value                                                       */
} IMPAI_ARMY;

/* THE BUY: the army `t` builds for value V against picture `p` from menu `m`. One function for the DLL and the
   tools, so the composition fights' armies are ImpAI's. No clock, no draw: a function of its arguments; every
   tie goes to the lower index. 0, or -1 when the menu holds more than IMPAI_MENU_MAX types, a type of a class
   past IMPAI_KS, or p->ne exceeds IMPAI_PIC_ENEMY (nothing is promised then). */
int impai_buy(const IMPAI_TABLE* t, const IMPAI_PICTURE* p, float V, const IMPAI_MENU* m,
              const IMPAI_PREDICTOR* pred, IMPAI_ARMY* out);

#endif
