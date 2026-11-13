#ifndef TASIM_KEYS_H
#define TASIM_KEYS_H
/* tasim_keys.h -- the definitions as the engine holds them, and the one rule per key that fills them.

   THE KEY TABLE (tasim_keys.c) has one row per key of the engine map's inventory (research/notes/engine/keys.md): the
   unit FBI's, the move class's, the weapon TDF's, the feature's and the map OTA's, and the keys Impure's DLL adds.
   A row names the key, the engine's offset in its record, the getter that reads it, the default the call pushes and
   the landing that models the rule reading the field. tasim_def_key is the one conversion of a key into a field, so
   every loader -- the install loader in tasim_run, defcheck.py's comparison, later the wiki's castings -- turns a
   key into the value the engine would hold (research/notes/impai/shared-core.md, "one rule per key").

   THE DEFINITIONS hold every field the inventory gives a destination, in the engine's type and unit: 16.16 where
   0x4C4800 converts, a float where the parser stores one, a word or a byte where it stores one. A reference the
   engine keeps as a pointer (a weapon, a feature, a move class, a category mask) is held as the name the file gives
   and resolved by index, never by address.

   ENGINE-FREE: no globals, no Win32, no file access, no engine memory. A loader hands each row its section's
   value through a TS_GET; the rows run in the parser's own order, so a default made from an earlier key (moverate1
   from maxvelocity, cloakcostmoving from cloakcost, objectname from unitname) sees that key's value as the engine's
   does. Doubles are rounded once per operation (SSE2, the Makefile's tasim_*.c rule), as the engine's x87 rounds
   them at the 53-bit precision the load runs at [INFERRED: the loader thread starts with Windows' default control
   word, 53-bit, round to nearest; _ftol sets its own chop]. */
#include <stddef.h>
#include <stdint.h>

/* ---- the three number getters (scout-Jb J7; research/notes/engine/keys.md, The getters) ------------------------
   A present key's value text. Each is what the engine's getter returns for a PRESENT key; an absent key takes the
   call's default, which tasim_def_key supplies (a present key whose text is no number is 0, never the default). */

/* 0x4C46C0 -> atoi 0x4E4F70 -> atol 0x4E4ED0, DISASSEMBLED: skip isspace (the C locale's 0x09..0x0D and 0x20), one
   optional sign, decimal digits accumulated n = n * 10 + d in 32 bits with no overflow test, so it wraps; negated
   for '-'. "1.6" -> 1, "-0.5" -> 0, "abc" -> 0, "4294967297" -> 1. */
int32_t ts_tdf_atol(const char* s);

/* 0x4C4760 -> atof 0x4E4560 -> _fltin 0x4EAF00, DISASSEMBLED to the decimal routines: skip isspace, then the
   correctly rounded double of [sign] digits [. digits] [{e|E|d|D} [sign] digits]; 0.0 when there are no digits
   (_fltin's flags 0x4, 0x4EAF25..0x4EAF3F). [INFERRED: the decimal-to-binary routines 0x4F3D10/0x4F3990 were not
   traced; they keep at most 21 significant digits in a 12-byte intermediate, so the correctly rounded value is
   theirs except within about 2^-60 of a tie or past 21 digits, which no install value comes near (J7). The d/D
   exponent is the grammar Microsoft documents for its CRT's atof and strtod.] Past 40 significant digits the rest
   only decides a tie (a sticky digit), and an exponent past +-99999 saturates. Overflow is +-infinity, as
   _ld12tod's is [INFERRED]. */
double ts_tdf_atof(const char* s);

/* 0x4C4800, DISASSEMBLED 0x4C4800..0x4C4898: atof, fmul by the double 65536.0 at 0x4FDC20 (exact, a power of two),
   _ftol 0x4E43A0 (chop, fistp QWORD, the low dword): truncation toward zero wrapped to 32 bits, 0 out of the int64
   range. "1.6" -> 104857, "-0.5" -> -32768, "-1.6" -> -104857, "32768" -> INT32_MIN. */
int32_t ts_tdf_fx(const char* s);

/* ---- the definitions -------------------------------------------------------------------------------------------
   Field comments give the engine's offset in its record. A name field is as long as the buffer the engine's string
   reader 0x4C48C0 is given (it keeps at most size - 1 characters). */

#define TS_NAMELEN      100     /* the 0x64-byte buffers the FBI reader names a reference in               */
#define TS_WNAMELEN     128     /* the 0x80-byte buffer a weapon name is read into (0x42CDD2)              */
#define TS_YARDLEN      1024    /* the 0x400-byte buffer of YardMap (0x42CF47)                             */
#define TS_UNIT_WEAPONS 10      /* weapon1..weapon3 the engine's, weapon4..weapon10 Impure's (tagpu_weapons.c) */
#define TS_CLASSES      32      /* moveinfo's CLASS0..CLASS31, the records 0x512358..0x512758              */
#define TS_WEAPON_IDS   4096    /* the weapon array's slots in Impure's raised build (TAGPU_LIM_WEAPONS)   */
#define TS_NONE         (-1)    /* a reference that names nothing                                          */

/* the 0x20-byte move class record, filled by 0x440340 from a CLASS<n> section or from a unit's own FBI section
   over the defaults 0x4402E0 sets */
typedef struct TASIM_MOVECLASS {
    char     name[TS_NAMELEN];  /* +0x00, a pointer to a copy of the section's `name` (0x42D365..0x42D392)     */
    int16_t  footx, footz;      /* +0x04, +0x06                                                             */
    int16_t  maxwaterdepth;     /* +0x08, default 10000                                                     */
    int16_t  minwaterdepth;     /* +0x0A, default -10000                                                    */
    uint8_t  maxslope;          /* +0x0C, default 255                                                       */
    uint8_t  badslope;          /* +0x0D, default (uint8)maxslope >> 1                                      */
    uint8_t  maxwaterslope;     /* +0x0E, default 255                                                       */
    uint8_t  badwaterslope;     /* +0x0F, default (uint8)maxwaterslope >> 1                                 */
    uint8_t  present;           /* the section exists (the core's, not the engine's)                         */
} TASIM_MOVECLASS;

typedef struct TASIM_UNITDEF {
    char     name[32];          /* +0x000, through the language reader 0x4C58A0                            */
    char     unitname[32];      /* +0x020                                                                   */
    char     description[64];   /* +0x040, through 0x4C58A0                                                 */
    char     objectname[32];    /* +0x080, default the unitname                                             */
    int16_t  footx, footz;      /* +0x14A, +0x14C, from the class record                                    */
    char     yardmap[TS_YARDLEN]; /* +0x14E holds the cells; this is the key's text (tasim_yard_cells)       */
    int32_t  box[7];            /* +0x15E, +0x166, +0x16A, +0x172, +0x176, +0x17E, +0x182: tasim_unitdef_derive */
    float    buildcostenergy;   /* +0x186, read as an int                                                   */
    float    buildcostmetal;    /* +0x18A                                                                   */
    int32_t  maxvelocity;       /* +0x192, 16.16                                                            */
    int32_t  slopevelocity;     /* +0x196, maxvelocity / (maxslope + 1), tasim_unitdef_derive               */
    int32_t  brakerate;         /* +0x19A, 16.16                                                            */
    int32_t  acceleration;      /* +0x19E                                                                   */
    int32_t  bankscale;         /* +0x1A2, default 1.0                                                      */
    int32_t  pitchscale;        /* +0x1A6                                                                   */
    int32_t  damagemodifier;    /* +0x1AA, default 1.0                                                      */
    int32_t  moverate1;         /* +0x1AE, default 2 maxvelocity                                            */
    int32_t  moverate2;         /* +0x1B2, default 2 maxvelocity                                            */
    char     movementclass[TS_NAMELEN]; /* +0x1B6 points at the class this names                             */
    int32_t  classidx;          /* that class's index, TS_NONE for none: the engine's NULL                  */
    uint16_t turnrate;          /* +0x1BA                                                                   */
    char     corpse[TS_NAMELEN]; /* +0x1BC holds the feature's index, 0xFFFF for none                        */
    int16_t  maxwaterdepth;     /* +0x1BE, from the class record                                            */
    int16_t  minwaterdepth;     /* +0x1C0                                                                   */
    float    energymake;        /* +0x1C2                                                                   */
    float    energyuse;         /* +0x1C6                                                                   */
    float    metalmake;         /* +0x1CA                                                                   */
    float    extractsmetal;     /* +0x1CE                                                                   */
    float    windgenerator;     /* +0x1D2                                                                   */
    float    tidalgenerator;    /* +0x1D6                                                                   */
    float    cloakcost;         /* +0x1DA, read as an int                                                   */
    float    cloakcostmoving;   /* +0x1DE, default the cloakcost's int                                      */
    int32_t  cloakcost_read;    /* that int, which the default is made from (no engine field)               */
    float    energystorage;     /* +0x1E2                                                                   */
    float    metalstorage;      /* +0x1E6                                                                   */
    int32_t  buildtime;         /* +0x1EA                                                                   */
    char     weapon[TS_UNIT_WEAPONS][TS_WNAMELEN]; /* +0x1EE, +0x1F2, +0x1F6 point at weapon1..3's records    */
    int32_t  weaponid[TS_UNIT_WEAPONS]; /* their IDs, TS_NONE for the null record: tasim_unitdef_weapons      */
    uint32_t maxdamage;         /* +0x1FA                                                                   */
    uint16_t workertime;        /* +0x1FE                                                                   */
    uint16_t healtime;          /* +0x200                                                                   */
    uint16_t sightdistance;     /* +0x202                                                                   */
    uint16_t radardistance;     /* +0x204                                                                   */
    uint16_t sonardistance;     /* +0x206                                                                   */
    uint16_t mincloakdistance;  /* +0x208, 80 when the type cloaks and the key left it 0                    */
    uint16_t radardistancejam;  /* +0x20A                                                                   */
    uint16_t sonardistancejam;  /* +0x20C                                                                   */
    char     soundcategory[TS_NAMELEN]; /* +0x20E holds the sound category's index                           */
    uint16_t buildangle;        /* +0x210                                                                   */
    uint16_t builddistance;     /* +0x212                                                                   */
    uint16_t maneuverleashlength; /* +0x214                                                                 */
    uint16_t attackrunlength;   /* +0x216                                                                   */
    uint16_t kamikazedistance;  /* +0x218                                                                   */
    uint16_t sortbias;          /* +0x21A                                                                   */
    uint16_t cruisealt;         /* +0x21C                                                                   */
    char     explodeas[TS_WNAMELEN];      /* +0x220, a weapon record                                       */
    char     selfdestructas[TS_WNAMELEN]; /* +0x224                                                        */
    uint8_t  maxslope;          /* +0x228, from the class record                                            */
    uint8_t  maxwaterslope;     /* +0x229                                                                   */
    uint8_t  transportsize;     /* +0x22A                                                                   */
    uint8_t  transportcapacity; /* +0x22B                                                                   */
    uint8_t  waterline;         /* +0x22C                                                                   */
    uint8_t  makesmetal;        /* +0x22D                                                                   */
    uint8_t  bmcode;            /* +0x22F, 0 a building                                                     */
    char     defaultmissiontype[TS_NAMELEN]; /* +0x230 holds the action's index (0x438760)                  */
    char     badtarget[3][TS_NAMELEN];       /* +0x231, +0x235, +0x239: wpri_, wsec_, wspe_badTargetCategory */
    char     nochase[TS_NAMELEN];            /* +0x23D, noChaseCategory                                     */
    char     category[TS_NAMELEN];           /* no field: the def's row bit in each named category's mask    */
    uint32_t flags241;          /* +0x241                                                                   */
    uint32_t flags245;          /* +0x245                                                                   */
    char     side[TS_NAMELEN];  /* read by the pre-scan 0x42A8D0 and the build menus                        */
} TASIM_UNITDEF;

/* the 0x115-byte weapon record, filled by 0x42E440 */
typedef struct TASIM_WEAPONDEF {
    char     section[32];       /* +0x000, the section's name, by which a unit names the weapon             */
    char     name[64];          /* +0x020                                                                   */
    int32_t  id;                /* the record's slot, the `ID` key (default -1, which no slot is)           */
    int32_t  weaponvelocity;    /* +0x068, x 65536/30 into 16.16 a tick                                     */
    int32_t  startvelocity;     /* +0x06C                                                                   */
    int32_t  weaponacceleration; /* +0x070, x 65536/900                                                     */
    char     model[256];        /* +0x074 holds the 3DO                                                     */
    float    energypershot;     /* +0x0C0                                                                   */
    float    metalpershot;      /* +0x0C4                                                                   */
    float    minbarrelangle;    /* +0x0C8, x pi/180 (the exe's constant), default -11.25 degrees            */
    int32_t  shakemagnitude;    /* +0x0CC                                                                   */
    int32_t  shakeduration;     /* +0x0D0, seconds x 30                                                     */
    uint16_t damage;            /* +0x0D4, [DAMAGE]'s `default`; 0 without the sub-section                  */
    uint16_t areaofeffect;      /* +0x0D6                                                                   */
    float    edgeeffectiveness; /* +0x0D8                                                                   */
    int32_t  range;             /* +0x0DC, default 0x7FFF                                                   */
    int32_t  coverage;          /* +0x0E0                                                                   */
    uint16_t reloadtime;        /* +0x0E4, seconds x 30                                                     */
    uint16_t weapontimer;       /* +0x0E6                                                                   */
    uint16_t turnrate;          /* +0x0E8, x 1/30                                                           */
    uint16_t burst;             /* +0x0EA                                                                   */
    uint16_t burstrate;         /* +0x0EC, seconds x 30                                                     */
    uint16_t sprayangle;        /* +0x0EE                                                                   */
    uint16_t duration;          /* +0x0F0, seconds x 30                                                     */
    uint16_t randomdecay;       /* +0x0F2                                                                   */
    uint16_t smokedelay;        /* +0x0FA                                                                   */
    uint16_t flighttime;        /* +0x0FC                                                                   */
    uint16_t holdtime;          /* +0x0FE                                                                   */
    uint16_t accuracy;          /* +0x104                                                                   */
    uint16_t tolerance;         /* +0x106                                                                   */
    uint16_t pitchtolerance;    /* +0x108                                                                   */
    uint8_t  firestarter;       /* +0x10B                                                                   */
    uint8_t  rendertype;        /* +0x10C                                                                   */
    uint8_t  color;             /* +0x10D                                                                   */
    uint8_t  color2;            /* +0x10E                                                                   */
    uint32_t flags;             /* +0x111                                                                   */
    uint8_t  wkeys;             /* Impure's weapon keys, tagpu_datakeys.h's TAGPU_WK_* bits: a table the DLL
                                   keeps by ID beside the record (tagpu_datakeys_weapon_read), no engine field */
    int32_t  damage_rows;       /* [DAMAGE]'s per-type rows, the table at +0x64 (T2): how many              */
} TASIM_WEAPONDEF;

/* the 0x100-byte FeatureDef, filled by 0x4224B0 */
typedef struct TASIM_FEATUREDEF {
    char     name[128];         /* +0x00, the section's name                                                */
    char     description[20];   /* +0x80                                                                    */
    int16_t  footx, footz;      /* +0x94, +0x96                                                             */
    char     object[256];       /* +0x98 holds the 3DO; with one, none of the GAF keys is read              */
    char     filename[256];     /* the GAF keys: anims\<filename>.GAF and its sequences (+0xA8..+0xC8)      */
    char     seq[8][256];       /* seqname, seqnameshad, seqnameburn, ..burnshad, ..die, ..dieshad,
                                   ..reclamate, ..reclamateshad                                              */
    char     burnweapon[256];   /* +0xE4, a weapon record                                                   */
    uint16_t sparktime;         /* +0xE8, seconds x 30                                                      */
    uint16_t damage;            /* +0xEA                                                                    */
    float    energy;            /* +0xEC, the int's low 16 bits                                             */
    float    metal;             /* +0xF0, the int's low 16 bits                                             */
    uint8_t  height;            /* +0xFA                                                                    */
    uint8_t  spreadchance;      /* +0xFB                                                                    */
    uint8_t  reproduce;         /* +0xFC                                                                    */
    uint8_t  reproducearea;     /* +0xFD                                                                    */
    uint16_t flags;             /* +0xFE                                                                    */
    char     successor[3][TS_NAMELEN]; /* featuredead, featurereclamate, featureburnt (0x422EA0)             */
} TASIM_FEATUREDEF;

/* ---- the key table ------------------------------------------------------------------------------------------ */

enum {                          /* how a row's value becomes its field (the getter in brackets)               */
    TSK_INT,                    /* [0x4C46C0] atol, its low `width` bytes                                    */
    TSK_INT_FLT,                /* [0x4C46C0] fild, fstp DWORD: the int as a float                           */
    TSK_INT_U16FLT,             /* [0x4C46C0] & 0xFFFF, fild, fstp DWORD                                     */
    TSK_FX,                     /* [0x4C4800] 16.16                                                          */
    TSK_DBL_FLT,                /* [0x4C4760] fstp DWORD                                                     */
    TSK_DBL_FTOL,               /* [0x4C4760] fmul `scale`, _ftol, its low `width` bytes                     */
    TSK_DBL_FLT_SC,             /* [0x4C4760] fmul `scale`, fstp DWORD                                       */
    TSK_BIT,                    /* [0x4C46C0] & 1, into bit `bit` of the `width`-byte word                   */
    TSK_BIT2,                   /* [0x4C46C0] & 3, into bits `bit`, `bit` + 1 of the `width`-byte word       */
    TSK_STR,                    /* [0x4C48C0] at most width - 1 characters, the rest of the field zeroed     */
    TSK_SDCOUNT,                /* [0x4C4630] atoi & 7 into +0x245 bits 20..22; 5 when absent               */
    TSK_KNOWN                   /* read by the engine (or Impure's DLL) into nothing the core holds          */
};

enum {                          /* the default's rule                                                       */
    TSR_CONST,                  /* `dflt` (an int kind) or `dfltd` (a double kind); "" for a string          */
    TSR_TWICE,                  /* 2 x the int32 field at `from`, in 32 bits (0x42C1E6: shl eax,1)           */
    TSR_COPY                    /* the int32 field at `from`, or for a string the string at `from`           */
};

enum {                          /* the landing whose rules read the field (research/notes/impai/landings.md) */
    TSL_T1A,                    /* the definitions and the world: identity, footprint, placement             */
    TSL_T1B,                    /* movement and pathing                                                      */
    TSL_T1C,                    /* economy and work                                                          */
    TSL_T1D,                    /* sensors                                                                   */
    TSL_T2,                     /* combat: weapons, damage, targeting                                        */
    TSL_T3,                     /* air, transports, cloak, stealth                                           */
    TSL_T4,                     /* Impure's engine and the mods                                              */
    TSL_AI,                     /* the stock AI's keys                                                       */
    TSL_OUT,                    /* presentation, the menus, the network: outside the simulation              */
    TSL_COUNT
};

#define TS_NOOFF 0xFFFFu        /* a row with no engine field                                                */

typedef struct TS_KEY {
    const char* name;           /* the key, matched without case as the getters' _stricmp matches             */
    uint16_t    off;            /* the engine's offset in its record, TS_NOOFF for none                      */
    uint16_t    field;          /* offsetof into the definition                                              */
    uint16_t    width;          /* the bytes stored: 1, 2 or 4; a string's buffer                            */
    uint8_t     kind;           /* TSK_*                                                                      */
    uint8_t     bit;            /* TSK_BIT's bit                                                             */
    uint8_t     rule;           /* TSR_*                                                                      */
    uint8_t     landing;        /* TSL_*                                                                      */
    uint16_t    from;           /* TSR_TWICE / TSR_COPY: the field the default is made from                  */
    int32_t     dflt;           /* an int kind's default, as the call pushes it (16.16 for TSK_FX)           */
    double      dfltd;          /* a double kind's default                                                   */
    double      scale;          /* TSK_DBL_FTOL / TSK_DBL_FLT_SC: the exe's constant                         */
} TS_KEY;

/* Each table in its parser's order; a row whose name is NULL ends it. */
extern const TS_KEY tasim_keys_unit[];      /* 0x42BF40, [UNITINFO], then the pre-scan's and Impure's keys     */
extern const TS_KEY tasim_keys_class[];     /* 0x440340, a CLASS<n> section or a unit's own                     */
extern const TS_KEY tasim_keys_weapon[];    /* 0x42E440, a weapon's section, then Impure's keys                 */
extern const TS_KEY tasim_keys_feature[];   /* 0x4224B0 and the successors' 0x422EA0                            */
extern const TS_KEY tasim_keys_map[];       /* 0x435DA0, read by the map loader to come (T1a's a5)              */

/* The row of `table` for `key` (without case), or NULL: a key no row has is one the engine does not read. */
const TS_KEY* tasim_keys_find(const TS_KEY* table, const char* key);

/* THE ONE RULE PER KEY. Stores `value` -- the section's text for the key, or NULL when the key is absent -- into
   `def` (the table's definition struct) as the engine's getter and the store after it would. Returns 1 for a
   present key, 0 for a default, -1 for a TSK_KNOWN row (nothing stored). */
int tasim_def_key(void* def, const TS_KEY* k, const char* value);

/* A section's value for a key, or NULL when absent: a loader's lookup (tagpu_tdf_get). */
typedef const char* (*TS_GET)(void* ctx, const char* key);

/* The class record's defaults, 0x4402E0 (DISASSEMBLED): footprint 0, depths 10000 and -10000, every slope 255. */
void tasim_moveclass_init(TASIM_MOVECLASS* c);

/* 0x440340 over `c` as it stands: its keys, then the clamps 0x4403F4..0x440418 (maxslope to maxwaterslope,
   badslope to maxslope, badwaterslope to maxwaterslope, unsigned). */
void tasim_moveclass_read(TASIM_MOVECLASS* c, TS_GET get, void* ctx);

/* 0x42BF40's keys in its order, then its tail: the move class (a `movementclass` naming one of `classes` copies
   that record, otherwise the unit's own section is read through 0x440340 over 0x4402E0's defaults,
   0x42CD0C..0x42CD97), +0x196 and the footprint box (tasim_unitdef_derive), mincloakdistance's 80, and Impure's
   keys. The weapon references stay names until tasim_unitdef_weapons. */
void tasim_unitdef_read(TASIM_UNITDEF* d, TS_GET get, void* ctx, const TASIM_MOVECLASS classes[TS_CLASSES]);

/* The fields 0x42BF40 computes from the read ones, DISASSEMBLED: +0x196 = (maxvelocity << 16) / ((maxslope + 1)
   << 16) through _allshl and _alldiv (0x42CD9D..0x42CDC7), so the truncated quotient of the two; and the footprint
   box at 0x42D079..0x42D11F, in 16.16, each half truncated toward zero: +0x15E = -(footx << 20) / 2, +0x166 =
   -(footz << 20) / 2, +0x16A = (footx << 20) / 2, +0x172 = (footz << 20) / 2, +0x176 = +0x16A - +0x15E,
   +0x17E = +0x172 - +0x166, +0x182 = (+0x176 + +0x17E) / 3 (the 0x55555556 multiply, truncated). +0x162,
   +0x16E and +0x17A take values from outside the parser and are not held. */
void tasim_unitdef_derive(TASIM_UNITDEF* d);

/* Each weapon name (weapon1..10, explodeas, selfdestructas, a feature's burnweapon) resolved as 0x49E5B0 resolves it:
   an empty name is the null record; else the lowest slot whose section name is equal without case, or the null
   record when none is (DISASSEMBLED 0x49E5B0..0x49E603). `ids[s]` is the record in slot s, NULL for an empty one.
   Sets +0x241 bit 16 when weapon1..3 are not all the null record (0x42CEF2..0x42CF24). */
int32_t tasim_weapon_lookup(const TASIM_WEAPONDEF* const* ids, int nids, const char* name);
void    tasim_unitdef_weapons(TASIM_UNITDEF* d, const TASIM_WEAPONDEF* const* ids, int nids);

/* 0x42E440's keys in its order for the section `section`, then [DAMAGE]'s `default` through `damage` (NULL when
   the section has no [DAMAGE]); the per-type rows are the caller's to count. The ID, the record's slot, is the
   `ID` key. Impure's build skips a section whose ID is outside 0..TS_WEAPON_IDS-1 or whose name does not fit its
   record (tagpu_patches.c, wpn_loader_id), which the caller tests with tasim_weapon_fits. */
void tasim_weapondef_read(TASIM_WEAPONDEF* w, const char* section, TS_GET get, void* ctx, TS_GET damage, void* dctx);
int  tasim_weapon_fits(const TASIM_WEAPONDEF* w, const char* section);

/* 0x4224B0's keys in its order for the section `section`, then the successors 0x422EA0 reads. */
void tasim_featuredef_read(TASIM_FEATUREDEF* f, const char* section, TS_GET get, void* ctx);

/* The cells of +0x14E for a building (bmcode 0), as Impure's yard parse fills them (tagpu_patches.c, yard_parse,
   the yardmap feature): footx x footz bytes, row by row; a char of `.CGOYcfowy` fills one cell, any other is
   skipped, the last char before the string's end repeats, and cells past the end take the last valid char's byte
   or 'o''s 0x2F. Returns the cell count, 0 for a mobile type (+0x14E NULL) or a footprint past `cap`. */
int tasim_yard_cells(const TASIM_UNITDEF* d, uint8_t* out, int cap);

/* ---- the named-not-modelled report -----------------------------------------------------------------------------
   What a load met that the core does not model, one row per (class, kind, what), with how often and where first. */

enum {
    TSRP_LATER,                 /* a key read and modelled by a later landing, or outside the simulation     */
    TSRP_IGNORED,               /* a key in a file that no parser reads: the engine ignores it too           */
    TSRP_REFUSED,               /* a value the engine would fault or hang on, which the core refuses         */
    TSRP_ENGINE                 /* an engine behaviour outside T1 (tools/tasim/engine-features.tsv)          */
};

#define TS_REPORT_MAX  1024
#define TS_REPORT_TEXT 128

typedef struct TS_REPORT_ROW {
    uint8_t cls;                /* TSRP_*                                                                    */
    uint8_t landing;            /* TSL_*: who models it                                                      */
    char    kind[12];           /* "unit", "weapon", "feature", "class", "map", "engine"                     */
    char    what[TS_REPORT_TEXT];  /* the key, the refusal or the feature                                   */
    char    first[TS_REPORT_TEXT]; /* the first definition or file it was met in                            */
    int32_t count;
} TS_REPORT_ROW;

typedef struct TS_REPORT {
    int32_t       n, dropped;   /* rows held, and rows past TS_REPORT_MAX counted only                        */
    TS_REPORT_ROW row[TS_REPORT_MAX];
} TS_REPORT;

void tasim_report_add(TS_REPORT* r, int cls, int landing, const char* kind, const char* what, const char* first);

/* Adds the TSRP_REFUSED rows a unit earns (research/notes/impai/tasim.md, the report's third class): a ground type
   (a mobile type, bmcode not 0, that can move and does not fly) with turnrate 0 or a brakerate whose 32-bit double
   is 0 (the ground step divides by both, 0x43D022 and 0x43D06C, and faults), a footprint
   side past 64 cells (worldq.h's WQ_FOOT_MAX), a canhover type whose 16.16 maxvelocity truncates to 0 when halved
   (the hover bob 0x48A66A divides by it), a selfdestructcountdown above 5 (its reply indexes past its table).
   Returns how many. */
int tasim_unitdef_refusals(const TASIM_UNITDEF* d, TS_REPORT* r);

const char* tasim_landing_name(int landing);

#endif
