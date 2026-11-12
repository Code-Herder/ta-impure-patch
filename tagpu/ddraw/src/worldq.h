#ifndef WORLDQ_H
#define WORLDQ_H
/* worldq.h -- one read-and-order interface to a game world, answering for one seat: the game's engine
   (tagpu_worldq.c) or the shared simulation core (tasim_worldq.c). research/notes/impai/shared-core.md.

   THE SHAPE. A WORLDQ is an instance -- a const ops table, the implementation's context `self` and the seat --
   and the ops table is `static const` in each implementation, in .rdata. That is why it is a table and not a
   struct of callbacks: a struct of callbacks puts code addresses in every instance, which simulation state
   holding one would carry into a recording, while a table in .rdata is not a writable static, so the
   simulation-state check asks nothing of it (inc/tagpu_simreg.h, THE CLASSES). The DLL holds both
   implementations -- the in-game wiki runs the core, the game's UI the engine -- and the wiki's host holds the
   core alone, so the choice is a pointer and not a link.

   NEVER STORED. A WORLDQ is opened for one call on the caller's stack and never outlives it: `self` is the
   implementation's context on that same stack and dies with the call. Nothing stores one.

   ONE SEAT. Every op answers for `seat` alone: its own units, its own explored map, its own orders. No op
   allocates; every list is copied into the caller's buffer, with its capacity.

   DETERMINISM. No op reads a clock, the cursor or a random generator. Lists come in a fixed order: slots
   ascending, order nodes in list order, cells row-major. Arithmetic is integer, or the definitions' own floats
   compared and never computed with. */
#include <stdint.h>

#define WQ_SEATS     10
#define WQ_NAME      32
#define WQ_FOOT_MAX  64             /* a unit footprint's side, cells: the engine's yard bound (fits) */
#define WQ_TYPES_MAX 16384          /* the most unit types a world has, None's included */
#define WQ_LIST_MAX  16384          /* the nodes any walk of an order list reads, at most */
#define WQ_NONE      0xFFFFu        /* no slot, no type */

enum { WQ_KIND_NONE, WQ_KIND_MEX, WQ_KIND_GEO };   /* a resource building: what it extracts */

/* a unit type's flags, from the engine definition's words */
#define WQ_DF_BUILDER   0x01u   /* +0x241 bit 6: it builds (the page loader's, 0x419670's builder test)    */
#define WQ_DF_ASSIST    0x02u   /* +0x241 bit 6, +0x245 bit 9 and workertime +0x1FE nonzero: it guards a
                                   builder to help it (tagpu_resource.c's helper test)                     */
#define WQ_DF_VTOL      0x04u   /* +0x241 bit 11: its orders are the VTOL_ ones                            */
#define WQ_DF_RECLAIMER 0x08u   /* +0x245 bit 10, canreclamate: the resolver's first reclaim test          */
#define WQ_DF_MOVER     0x10u   /* +0x22F nonzero: a type with a move class, which places no building      */

typedef struct WQ_MAP {
    int32_t w, h;               /* cells of 16 px                                                          */
    int32_t threshold;          /* the metal a cell needs above it to count, 0..255                        */
    uint8_t sea;                /* the sea level byte                                                      */
    uint8_t lostype;            /* LosType: bit 1 = True line of sight                                     */
    int32_t list_cap;           /* the orders a unit's list takes, the world's own bound; 0 for none       */
} WQ_MAP;

typedef struct WQ_DEF {
    uint16_t type;
    int16_t  fx, fz;            /* the footprint as the definition holds it; a rule tests 1..WQ_FOOT_MAX    */
    uint8_t  kind;              /* WQ_KIND_*                                                               */
    uint8_t  yard;              /* it has a yard map (yard() answers it)                                   */
    uint32_t flags;             /* WQ_DF_*                                                                 */
    float    strength;          /* the definition's own float, for a MEX its ExtractsMetal, for a GEO its
                                   EnergyMake; 0 when not finite or not above 0. Compared, never computed  */
    uint16_t workertime;
    uint16_t reach;             /* builddistance, world px                                                 */
    int32_t  speed;             /* maxvelocity, 16.16 world px a tick                                      */
    char     name[WQ_NAME];     /* unitname, NUL-ended                                                     */
} WQ_DEF;

typedef struct WQ_FEAT {
    int32_t  ax, az;            /* the anchor cell                                                         */
    uint16_t def;               /* the feature definition, below their count                               */
    int16_t  fx, fz;            /* footprint, cells, held to 1..16                                         */
    uint8_t  flags;             /* FeatureDef +0xFE: 0x20 geothermal, 0x80 reclaimable                     */
    float    metal, energy;     /* FeatureDef +0xF0, +0xEC                                                 */
} WQ_FEAT;

typedef struct WQ_UNIT {        /* one of the seat's own live units                                        */
    uint16_t slot, type, index; /* type: 0 when the unit's definition is not one; index: UnitInGameIndex   */
    uint8_t  owner, finished;   /* finished: the build fraction left (+0x104) is exactly 0                 */
    int32_t  pos[3];            /* 16.16 x, altitude, z                                                    */
    uint32_t state;             /* the unit's flag word (+0x110)                                           */
} WQ_UNIT;

typedef struct WQ_ORDER {       /* a COPY of one order node: no pointer survives the call                  */
    uint16_t place;             /* 0-based place in the unit's list                                        */
    uint8_t  action, started;   /* started: the node's byte 6 bit 1                                        */
    uint8_t  build;             /* a placed build: the action's descriptor capability bit 0, a type below
                                   ndefs and a position with x and z above 0                               */
    uint8_t  owned;             /* the node owns an object (a nanoframe)                                   */
    uint16_t type;              /* build type, 0 none                                                      */
    uint16_t target;            /* the target unit's slot, WQ_NONE when none or not a unit                 */
    int32_t  pos[3];            /* 16.16                                                                   */
    uint32_t flags;             /* the node's flag word (+0x42), WQ_OF_*                                   */
} WQ_ORDER;

/* WQ_ORDER.flags' bits that a rule reads, DISASSEMBLED in 0x43ADC0's replace and link (0x43AE02..0x43AF84) and
   the node's destructor 0x43A1F0: engine/orders.md */
enum { WQ_OF_KEEP   = 1u << 2,  /* survives a replace                                                     */
       WQ_OF_DROP   = 1u << 14, /* a head the next append drops                                           */
       WQ_OF_SECOND = 1u << 18, /* the node belongs on the second list (+0x60)                            */
       WQ_OF_STOP   = 1u << 22  /* its destructor starts the StopBuilding script                          */
};

enum { WQ_CMD_NEW = 1,          /* 0x43ADC0: no Shift replaces the list but its 0x4 nodes, then appends    */
       WQ_CMD_CLICK,            /* 0x43AFC0: Shift takes off the first same-action node within 16 world
                                   units instead of adding one; no Shift replaces as NEW does              */
       WQ_CMD_REMOVE };         /* 0x439F80: exact removal of `expect`, found again by a fresh walk        */
typedef struct WQ_CMD {
    uint8_t  verb, action, shift;
    uint16_t unit, target;      /* slots; target WQ_NONE = none                                            */
    int32_t  pos[3];            /* 16.16                                                                   */
    uint16_t type;              /* build type or 0                                                         */
    WQ_ORDER expect;            /* REMOVE: the node as `orders` copied it                                  */
} WQ_CMD;
enum { WQ_OK = 1, WQ_REFUSED = 0, WQ_UNSUPPORTED = -1 };

enum { WQ_ACT_MOBILEBUILD, WQ_ACT_VTOL_MOBILEBUILD, WQ_ACT_RECLAIM, WQ_ACT_VTOL_RECLAIM,
       WQ_ACT_RESURRECT, WQ_ACTS };
enum { WQ_CAN_GUARD = 7, WQ_CAN_RECLAIM = 12 };     /* the resolver's modes the rules ask */

typedef struct WQ_ECON { float metal, energy, metal_max, energy_max; } WQ_ECON;

/* ---- what a seat sees (ImpAI: research/notes/modules/impai.md, What a seat knows) ---- */

/* observe's classes, the first that holds winning: the seat's own unit; one its line of sight shows (owner and
   type known); one an ally's shared radar shows; one the seat's own radar pass found (position only) */
enum { WQ_SEEN_NONE, WQ_SEEN_OWN, WQ_SEEN_LOS, WQ_SEEN_SHARED, WQ_SEEN_CONTACT };
enum { WQ_LAYER_SURFACE, WQ_LAYER_AIR, WQ_LAYER_SUBMERGED };
typedef struct WQ_OBS {
    uint8_t  seen;              /* WQ_SEEN_*                                                               */
    uint8_t  owner;             /* 0..9; 0xFF when not identified (SHARED, CONTACT)                        */
    uint16_t type;              /* WQ_NONE when not identified                                             */
    uint16_t bx, bz;            /* the 32-px block of the unit's position, held to the map                 */
    uint8_t  layer;             /* WQ_LAYER_*                                                              */
} WQ_OBS;

/* A radar pass's answer, one nibble a slot (slot k: byte k / 2, the low nibble for an even k): the engine's
   +0x110 bits 8 and 9 as the pass would leave them for the seat, whether a jammer reached the unit (bit 10),
   and whether the pass's first loop kept the unit's bits (an own or shared unit), whose bit 10 the engine
   does not clear. */
#define WQ_RB_RADAR   0x1u
#define WQ_RB_SONAR   0x2u
#define WQ_RB_JAMMED  0x4u
#define WQ_RB_KEPT    0x8u
#define WQ_RADAR_BYTES(nslots) (((unsigned)(nslots) + 1u) / 2u)
static inline unsigned wq_radar_bits(const uint8_t* bits, unsigned slot)
{
    return (unsigned)(bits[slot / 2] >> ((slot & 1u) * 4u)) & 0xFu;
}
static inline void wq_radar_set(uint8_t* bits, unsigned slot, unsigned v)
{
    const unsigned sh = (slot & 1u) * 4u;
    bits[slot / 2] = (uint8_t)((bits[slot / 2] & ~(0xFu << sh)) | ((v & 0xFu) << sh));
}

/* A radar pass's progress: the CALLER's state, plain values, so a pass that spans calls can live in recorded
   state. Stage 0 is idle: the next call starts a pass. */
enum { WQ_RS_IDLE, WQ_RS_EMIT, WQ_RS_JAM };
typedef struct WQ_RADAR {
    uint8_t  stage;             /* WQ_RS_*                                                                 */
    uint8_t  pad;
    uint16_t cursor;            /* the next slot of the stage                                              */
    uint32_t start;             /* the gametime the pass began                                             */
} WQ_RADAR;

typedef struct WORLDQ WORLDQ;
typedef struct WORLDQ_OPS {
    int (*map)(const WORLDQ*, WQ_MAP* out);
    /* the seat has explored cell (cx, cz); 0 off the map */
    int (*explored)(const WORLDQ*, int cx, int cz);
    /* the cell's metal 0..255, -1 unexplored or off the map */
    int (*metal)(const WORLDQ*, int cx, int cz);
    /* the explored cells of the footprint above the threshold */
    int (*coverage)(const WORLDQ*, int cx, int cz, int fx, int fz);
    /* the seat's map holds the point, as the reclaim click's resolver asks it */
    int (*mapped_point)(const WORLDQ*, const int32_t pos[3]);
    /* the ground's height at a point, world px */
    int (*ground)(const WORLDQ*, const int32_t pos[3]);
    int (*ndefs)(const WORLDQ*);
    /* type 1..ndefs-1; 0 for none */
    int (*def)(const WORLDQ*, int type, WQ_DEF* out);
    /* the type's yard map, fx * fz bytes row-major; 0 when it has none or cap is short */
    int (*yard)(const WORLDQ*, int type, uint8_t* out, int cap);
    /* the types a builder type's build menu offers, ascending; their count, -1 when cap is short */
    int (*build_options)(const WORLDQ*, int builder_type, uint16_t* out, int cap);
    /* the site test, the instance's own (tagpu_worldq.h WQ_SITE_*), at first cell (cx, cz) with the unit
       `ignore_slot` taken as gone (0 none); with `alt` the free site's altitude, 0..255 */
    int (*site_ok)(const WORLDQ*, int type, int cx, int cz, int ignore_slot, int* alt);
    /* the altitude the build cursor gives a refused site */
    int (*blocked_alt)(const WORLDQ*, int type, int cx, int cz);
    /* the feature at cell (cx, cz), by its anchor; 0 for none. Any cell: a rule that respects the fog
       applies its own explored test */
    int (*feature)(const WORLDQ*, int cx, int cz, WQ_FEAT* out);
    int (*nslots)(const WORLDQ*);
    /* the slots the seat's units can hold, first..last inclusive */
    int (*seat_units)(const WORLDQ*, int* first, int* last);
    /* the seat's own live unit at the slot; 0 for any other slot. The visibility filter's first half, by
       construction: no rule reads another player's unit through it */
    int (*unit)(const WORLDQ*, int slot, WQ_UNIT* out);
    /* the unit's orders in list order, at most `cap` (every walk is bounded); their count, -1 for no unit */
    int (*orders)(const WORLDQ*, int slot, WQ_ORDER* out, int cap);
    /* the resolver's action byte for the unit, mode WQ_CAN_* at `target_slot` (WQ_NONE: the point), -1 when
       it refuses */
    int (*can)(const WORLDQ*, int slot, int mode, int target_slot, const int32_t pos[3]);
    /* WQ_ACT_* -> its action byte, -1 when the world has none */
    int (*action)(const WORLDQ*, int act);
    /* WQ_OK, or WQ_REFUSED without side effects: the unit is not the seat's, the target is no longer a
       live unit, or a REMOVE's node is not found again */
    int (*order)(const WORLDQ*, const WQ_CMD* cmd);
    /* WQ_OK, or WQ_UNSUPPORTED where the world keeps no such figures */
    int (*economy)(const WORLDQ*, WQ_ECON* out);
    int (*gametime)(const WORLDQ*);
    /* What the seat may know of the unit at `slot`, the first class that holds: WQ_SEEN_OWN, _LOS, _SHARED,
       or, when `contact` says the seat's own radar pass found the slot (radar_step's bits), _CONTACT; 0 for
       none, out zeroed. Another player's unit reaches a rule through here alone */
    int (*observe)(const WORLDQ*, int slot, int contact, WQ_OBS* out);
    /* the seat sees 32-px block (bx, bz) now; 0 off the map */
    int (*block_lit)(const WORLDQ*, int bx, int bz);
    /* Advances the seat's radar pass in `st` while *budget is above 0, a whole emitter's or jammer's walk at a
       time, and takes every target visit off *budget, so it ends at most one walk below 0; `bits` holds
       WQ_RADAR_BYTES(nslots) bytes. 1 when the pass is complete: `bits` holds its answer and `st` is idle */
    int (*radar_step)(const WORLDQ*, WQ_RADAR* st, uint8_t* bits, int nbytes, int32_t* budget);
    /* movement class `mclass` (0..WQ_MCLASSES-1) at cell (cx, cz), its footprint's first cell: 0 blocked, 1
       passable but bad, 3 good; 0 off the map and for a class the world does not have */
    int (*passable)(const WORLDQ*, int mclass, int cx, int cz);
} WORLDQ_OPS;

#define WQ_MCLASSES 32              /* the engine's movement classes, 0x512358 .. 0x512758 */

struct WORLDQ { const WORLDQ_OPS* ops; void* self; int seat; };

#endif
