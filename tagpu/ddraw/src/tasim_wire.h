#ifndef TASIM_WIRE_H
#define TASIM_WIRE_H
/* tasim_wire.c -- the scenario wire's parser: the line-oriented file `tacli scenario` writes from a scenario's
   JSON (research/notes/scenario-format.md, *Transport*) becomes a TASIM_WIRE, and nothing else. One copy, built
   into the DLL for its applier (tagpu_scenario.c) and natively for tasim; tests/tasim_wire_test.py holds the
   builds to one another. What a line means to a live game -- resolving names,
   bounding positions against the map, creating, ordering -- is each caller's.

   Engine-free: no globals, no Win32, no engine memory; the parse writes only the TASIM_WIRE and the error list
   its caller owns. Every bound is the caller's (the TASIM_WIRE's caps) or this header's. Its one floating-point
   step, a storage column's double rounded to a float, is rounded once in every build. */

#define TASIM_WIRE_UNSET      (-2147483647 - 1) /* the wire's `-` column: the engine's own default    */
#define TASIM_WIRE_NAMELEN    32                /* a type name, with its terminator; longer is cut    */
#define TASIM_WIRE_MAPLEN     128               /* the map line's name, with its terminator           */
#define TASIM_WIRE_PLAYERS    10                /* player lines, at most: the engine's ten seats      */
#define TASIM_WIRE_MAX_ERRORS 24                /* errors kept verbatim; the rest are counted         */
#define TASIM_WIRE_ERRLEN     168               /* one error, with its terminator                     */

/* a target kind of an order or of the camera; TASIM_TGT_NONE on the camera is "no cam line" */
#define TASIM_TGT_NONE 0
#define TASIM_TGT_POS  1
#define TASIM_TGT_UNIT 2
#define TASIM_TGT_FEAT 3
#define TASIM_TGT_COUNT 4                       /* an order's amount, `count <n>`, in `a`; no target  */

/* `buildweapon`: the stockpile build of slot 0's weapon, which is no command of the order mapper 0x43F0E0 (its
   types are 1..14), so it carries a number of its own outside them; the applier issues it through the queue root
   0x43B0B0, as the build menu does */
#define TASIM_CMD_BUILDWEAPON 256
#define TASIM_WIRE_STOCK_MAX  200               /* a stock byte and a buildweapon count, at most: the
                                                   stock BuildWeapon 0x402B70 stops at (0x402CCA)       */
#define TASIM_WIRE_AT_MAX     65535             /* a unit's `at=`, ticks after the apply, at most       */
#define TASIM_WIRE_SLOT_MAX   65535             /* a unit's `slot=`, a position in its owner's block, at
                                                   most: the create's slot request is a word (0x486006) */

/* WHO an order is for. An ordinal names a unit this same wire creates and is the only identity
   scenario-format.md calls public, because UnitInGameIndex is recycled when a unit dies. The other two let a
   caller order units that were already there (`tacli order`), and each carries its own guard against that
   recycling: SUBJ_LIVE carries the caller's claim about the slot's type, which the applier tests before it
   orders, and SUBJ_SEL names no slot at all, it asks the engine what the player has selected. */
#define TASIM_SUBJ_ORD  0                       /* an ordinal in this wire                            */
#define TASIM_SUBJ_LIVE 1                       /* a live UnitInGameIndex                             */
#define TASIM_SUBJ_SEL  2                       /* whatever is selected when it applies               */

/* `unit <ord> <type> <owner> <x> <y> <height> <facing> <hp%> <stance> <nano%> <kills> <fire> <stock>
    [at=<n>] [slot=<p>]`: the two keys optional, in that order. They are the engine lane's alone (the packed
   calibration lane). This parser takes them in every build; refusing them is the tasim_run interface's
   contract, which T1 builds where tasim's world consumes a wire: a wire with an `at` other than 0 or with a
   `slot` exits 2 with "scheduled and placed creates are the engine lane's". Until then
   tools/tasim/tests/fake_tasim.py stands in for tasim_run and refuses so
   (research/notes/modules/tasim-calibration.md, *The tasim_run interface*). */
typedef struct {
    char type[TASIM_WIRE_NAMELEN];
    int  owner, x, y;
    int  height, facing, health, stance, nano;  /* TASIM_WIRE_UNSET = the engine's default; stance is
                                                   0 hold, 1 manoeuvre, 2 roam                        */
    int  kills;                                 /* TASIM_WIRE_UNSET = the engine's 0                  */
    int  fire;                                  /* TASIM_WIRE_UNSET = the def's; 0 hold fire, 1 return
                                                   fire, 2 fire at will                               */
    int  stock;                                 /* TASIM_WIRE_UNSET = the create's 0; else
                                                   0..TASIM_WIRE_STOCK_MAX, slot 0's stock            */
    int  at;                                    /* ticks after the apply that the unit is created:
                                                   0..TASIM_WIRE_AT_MAX; 0 = at the apply             */
    int  slot;                                  /* the position in its owner's block,
                                                   0..TASIM_WIRE_SLOT_MAX; TASIM_WIRE_UNSET = the
                                                   first free slot, as every create without it        */
} TASIM_WIRE_UNIT;

/* `feat <ord> <type> <x> <y> <height> <facing>` */
typedef struct {
    char type[TASIM_WIRE_NAMELEN];
    int  x, y, height, facing;                  /* height, facing: TASIM_WIRE_UNSET = the default     */
} TASIM_WIRE_FEAT;

/* `order <subject> <command> [pos <x> <y> | unit <ord|@ref> | feat <ord> | count <n>] [after=<n>] [queue]`;
   `count` is buildweapon's, 1..TASIM_WIRE_STOCK_MAX, and buildweapon takes nothing else. `queue`, last on the
   line, joins the order to its subject's list as TA's Shift does: the parse takes it only on an ordinal's attack
   chain of `unit <ordinal>` targets (tasim_wire.c, queue_joins_chain) */
typedef struct {
    int  subj;                                  /* TASIM_SUBJ_*                                       */
    int  uord;                                  /* the ordinal, or the live UnitInGameIndex           */
    char sexp[TASIM_WIRE_NAMELEN];              /* the subject's type guard, "" = none                */
    int  cmd;                                   /* the engine's command type (tasim_wire.c's table)   */
    int  kind;                                  /* TASIM_TGT_*                                        */
    int  a, b;                                  /* pos x, y; the target's ordinal or index in a; or the
                                                   count in a                                         */
    int  tlive;                                 /* the unit target `a` is a live UnitInGameIndex      */
    char texp[TASIM_WIRE_NAMELEN];              /* the live target's type guard, "" = none            */
    int  after;                                 /* the ticks it waits, 0..255; 0 = at once            */
    int  queue;                                 /* 1: issued with Shift, joining its subject's list   */
} TASIM_WIRE_ORDER;

/* `player <slot> [metal=<n>] [energy=<n>] [ai=off] [mstore=<f> estore=<f>]` */
typedef struct {
    int   slot;                                 /* 0..9                                               */
    int   metal, energy;                        /* TASIM_WIRE_UNSET = unchanged                       */
    int   aioff;                                /* `ai=off`                                           */
    int   hasstore;                             /* bit 0 mstore=, bit 1 estore=: 0 or 3 once parsed   */
    float mstore, estore;                       /* finite and 0 or more                               */
} TASIM_WIRE_PLAYER;

/* The errors, in the order they were found. The applier appends its own to the parse's, so a caller reports
   one list. */
typedef struct {
    char msg[TASIM_WIRE_MAX_ERRORS][TASIM_WIRE_ERRLEN];
    int  n;                                     /* kept in msg                                        */
    int  dropped;                               /* found past TASIM_WIRE_MAX_ERRORS                   */
} TASIM_WIRE_ERRS;

/* One parsed wire. The caller owns the struct and its three arrays and sets the arrays and their caps; a
   parse refuses a wire with more entries than a cap, naming the cap. */
typedef struct {
    TASIM_WIRE_UNIT*  units;
    int               units_cap;
    TASIM_WIRE_FEAT*  feats;
    int               feats_cap;
    TASIM_WIRE_ORDER* orders;
    int               orders_cap;

    int               nunits, nfeats, norders;
    unsigned int      seed;                     /* `v1 seed=`, 0..0xFFFFFFFF                          */
    int               clear;                    /* `v1 clear=`: clear the existing units, default 1   */
    int               abort;                    /* `v1 onerror=`: 1 abort (default), 0 skip           */
    int               limit;                    /* `limit`; TASIM_WIRE_UNSET when absent              */
    char              map[TASIM_WIRE_MAPLEN];   /* `map`: the rest of the line, "" when absent        */
    unsigned          sw_set, sw_clr;           /* `sw`: SoftwareDebugMode bits to set and to clear   */
    TASIM_WIRE_PLAYER pl[TASIM_WIRE_PLAYERS];
    int               npl;
    int               clearfeat;                /* `clearfeat 0|1`                                    */
    int               objective;                /* `objective`: -1 absent, 0 off, 1 on                */
    int               cam_kind;                 /* `cam`: TASIM_TGT_*, TASIM_TGT_NONE when absent     */
    int               cam_a, cam_b;             /* pos x, y; or the target's ordinal in cam_a         */
    int               cam_pin;                  /* `cam ... pin=1`                                    */
} TASIM_WIRE;

/* SoftwareDebugMode's named bits, which `sw` sets and clears: { name, bit }, ended by { 0, 0 } */
typedef struct { const char* name; unsigned bit; } TASIM_WIRE_SWITCH;
extern const TASIM_WIRE_SWITCH tasim_wire_switches[];

/* Parses `text`, which it cuts into tokens in place, into `w` (its fields reset first, its arrays and caps
   kept), appending what is wrong to `errs`. 1 when the whole file parsed and ended with `end`; 0 after the
   first error, with every entry counted in `w` parsed whole. A truncated file is refused rather than half
   applied: that is what `end` is for. Once the whole file is read it refuses an order whose ordinal subject (a
   live or selected one counting as at=0) and ordinal unit target have different `at`. */
int  tasim_wire_parse(TASIM_WIRE* w, char* text, TASIM_WIRE_ERRS* errs);

/* `w`'s parsed fields to an empty wire's; its arrays and caps are kept */
void tasim_wire_reset(TASIM_WIRE* w);

void tasim_wire_errs_reset(TASIM_WIRE_ERRS* errs);

/* Appends one error, cut to TASIM_WIRE_ERRLEN - 1 characters, or counts it in `dropped` once the list is full. */
void tasim_wire_err(TASIM_WIRE_ERRS* errs, const char* fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* The wire's tokenizer: the next run of characters other than space and tab, terminated in place, and *p
   moved past it; NULL at the end of the line. */
char* tasim_wire_tok(char** p);

/* `name=value` with an integer value (`-` is TASIM_WIRE_UNSET): 1 with the token cut at its `=`; 0 when it has
   no `=`, and 0 with the token cut when the value is not a whole number. */
int  tasim_wire_kv(char* t, char** key, int* value);

#endif
