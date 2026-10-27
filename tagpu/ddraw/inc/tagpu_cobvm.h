#ifndef TAGPU_COBVM_H
#define TAGPU_COBVM_H
/* tagpu_cobvm -- the COB virtual machine, engine-free: the engine's runner 0x4B0DA0, its stepper
   0x4B1C00 and its entries 0x4B0940 / 0x4B0B00 / 0x4B0C40 / 0x4B0D60, opcode for opcode, on memory
   laid out as Impure's game lays it out (research/notes/ingame-wiki-cob-plan.md, "The core";
   exe-reverse-engineering.md, "The COB engine"). The one COB VM outside the engine: the in-game
   wiki, tools/tacob and the wiki mockup's server all run this file.

   ITS MEMORY IS THE ENGINE'S, in four blocks the caller owns:
     object   TAGPU_COB_OBJECT_BYTES, the COB object (tagpu_cob_safe.h): +0x04 the sim rate (30),
              +0x18 "something is animating", the records from +0x1C, the running count at
              TAGPU_COB_COUNT_OFFSET. Its pointer fields (+0x00, +0x08, +0x0C, +0x10, +0x14 and
              TAGPU_COB_MODEL_OFFSET) are never read or written here.
     statics  int32 a static variable (the block at *(cob+0x10))
     anim     TAGPU_COBVM_ANIM int32s a piece (the block at *(cob+0x14))
     model    TAGPU_COBVM_MODEL_BYTES(pieces): the posed model's header and its PrimitiveStructs
              (the Object3do at *(cob+0x540)), which MOVE, TURN, SHOW, CACHE and SHADE write
   so a block the game holds can be handed to it as it is.

   SAFE ON SEVERAL THREADS BY CONSTRUCTION: no global and no written static. Everything one call
   touches is its TAGPU_COBVM, its four blocks and its TAGPU_COBVM_RUN; two threads may run two
   VMs at once, never one VM. What a script reaches outside its own unit leaves through the host:
   reads as callbacks, writes as TAGPU_COBVM_EFFECT entries (the effect log).

   BOUNDED BY CONSTRUCTION: the program comes from tagpu_cob_program (tagpu_cob_safe.c), which
   validated every operand and branch, and every opcode passes tagpu_cob_check_step, the same
   guard the game's interpreter takes at 0x4B0E5D. A fault is a refusal: the call returns -1 with
   the reason in the run, and the VM is not run again. */
#include <stdint.h>
#include "tagpu_cob_safe.h"

#define TAGPU_COBVM_ANIM        19u     /* dwords a piece in the animation block (0x4B0756) */
#define TAGPU_COBVM_PRIM_BASE   0x22u   /* the first PrimitiveStruct in the model (0x480C40) */
#define TAGPU_COBVM_PRIM_BYTES  0x36u   /* a PrimitiveStruct */
#define TAGPU_COBVM_MODEL_BYTES(pieces) (TAGPU_COBVM_PRIM_BASE + (pieces) * TAGPU_COBVM_PRIM_BYTES)

/* In a PrimitiveStruct: three 16.16 positions at +0x04, three uint16 angles at +0x10, a word at
   +0x26 every pose change clears, and the flags word at +0x28 (0x480C50..0x480E87). */
#define TAGPU_COBVM_PRIM_POS    0x04u
#define TAGPU_COBVM_PRIM_TURN   0x10u
#define TAGPU_COBVM_PRIM_STAMP  0x26u
#define TAGPU_COBVM_PRIM_FLAGS  0x28u
#define TAGPU_COBVM_VISIBLE     0x1u
#define TAGPU_COBVM_CACHED      0x2u
#define TAGPU_COBVM_SHADED      0x4u

/* The rules of the game's two builds. RETAIL is the retail engine, which the stock-limits build
   runs unpatched: eight records, and a refused START leaves its arguments on the parent's stack.
   IMPURE is the raised build's guarded interpreter: TAGPU_COB_THREADS records, and a refused START
   drops them (tagpu_cob_parent_pops). */
enum { TAGPU_COBVM_RETAIL = 0, TAGPU_COBVM_IMPURE = 1 };

/* Where a thread start came from, for the trace: the engine, a START, a CALL. */
enum { TAGPU_COBVM_FROM_ENGINE = 'E', TAGPU_COBVM_FROM_START = 'C', TAGPU_COBVM_FROM_CALL = 'L' };

/* One write a script makes outside its own unit, in push order: `op` the opcode
   (word & 0x100FF000) and its operands.
     SET        0x10082000  v[0] the value id, v[1] the value
     EMIT_SFX   0x1000F000  piece, v[0] the effect type
     EXPLODE    0x10071000  piece, v[0] the flags
     ATTACH     0x10083000  v[0] the unit id, v[1] the piece, v[2] the third word
     DROP       0x10084000  v[0] the unit id */
typedef struct TAGPU_COBVM_EFFECT {
    uint32_t op;
    int32_t  slot, pc, piece;
    int32_t  v[3];
} TAGPU_COBVM_EFFECT;

/* One line of the cobtrace oracle (tacob-design.md, "The trace contract"), as fields. */
typedef struct TAGPU_COBVM_TRACE {
    char           kind;        /* 'S' start, 'R' return, 'K' kill, 'D' draw, 'X' refused start */
    uint32_t       tick;
    int32_t        slot;        /* S, R, K, D: the record; K: the killed one */
    int32_t        script;      /* S, X: the script's index; R, K: -1 (the host maps `pc`) */
    int32_t        pc;          /* R: the RETURN's pc; K: the killed record's pc */
    char           from;        /* S, X: TAGPU_COBVM_FROM_* */
    int32_t        parent;      /* S: the starting record, -1 for the engine; K: the killer */
    int32_t        argc;        /* S: the arguments' count */
    const int32_t* args;        /* S: the arguments, on the new record */
    int32_t        value;       /* R: the returned value; D: the drawn value */
    int            has_value;   /* R: 0 when the stack is empty ("?") */
} TAGPU_COBVM_TRACE;

typedef struct TAGPU_COBVM_HOST {
    void* ctx;
    /* GET_UNIT_VALUE and GET (vt+0x44, 0x480770), every value id */
    int32_t (*get)(void* ctx, int32_t id, int32_t a, int32_t b, int32_t c, int32_t d);
    /* RAND's draw (0x4B6C30): 0 .. n-1, and 0 for n < 2. `lo` is the script's lower bound, which
       the engine does not pass: an oracle replay hands back the logged `lo + r` through it. */
    int32_t (*rand)(void* ctx, int32_t n, int32_t lo);
    /* 0x10044000 (vt+0x48, 0x481430): 1 when unit `id` is on the unit's list at +0x8A */
    int32_t (*cargo)(void* ctx, int32_t id);
    /* 0x10045000 (vt+0x4C, 0x481470): the id of the unit at +0x86, 0 for none */
    int32_t (*carrier)(void* ctx);
    /* IMPURE: an ATTACH or DROP naming a unit that is not alive is skipped, its words popped */
    int (*alive)(void* ctx, int32_t id);
    /* RETURN with a completion object, and a refused 0x4B0B00 start that has one (with 0) */
    void (*callback)(void* ctx, uint32_t cb, int32_t value);
    /* a write, at its opcode, before the next opcode runs; NULL keeps it in the run's log */
    void (*commit)(void* ctx, const TAGPU_COBVM_EFFECT* e);
    /* the oracle's lines; NULL for none */
    void (*trace)(void* ctx, const TAGPU_COBVM_TRACE* t);
} TAGPU_COBVM_HOST;

/* One call's context: the host, the effect log (when the host has no commit), the tick the trace
   stamps, and the reason of a fault. */
typedef struct TAGPU_COBVM_RUN {
    const TAGPU_COBVM_HOST* host;
    TAGPU_COBVM_EFFECT*     log;
    unsigned                cap, n;
    uint32_t                tick;
    char                    reason[160];
} TAGPU_COBVM_RUN;

/* One unit's VM. The pointers are the caller's; the latch is the trace's start held until the
   next event, as the oracle writes it (the callers push a start's arguments after it returns). */
typedef struct TAGPU_COBVM {
    const TagpuCobProgram* program;
    unsigned char*         object;
    int32_t*               statics;
    int32_t*               anim;
    unsigned char*         model;
    unsigned               threads, rules;
    int                    faulted;
    struct { int on, slot, script, parent, argc; char from; uint32_t tick; } pend;
} TAGPU_COBVM;

/* Bind the blocks and clear them as the engine's constructor and allocations leave them: the
   object's records, count and flag zero, its rate 30, statics and animation zero. The model is
   the caller's to fill (the model builder's initial flags, 0x45AEC0). `program` must have had
   tagpu_cob_bind_model with the model's piece count. Returns 1, or 0 with the reason. */
int tagpu_cobvm_init(TAGPU_COBVM* vm, const TagpuCobProgram* program, unsigned rules,
                     unsigned char* object, int32_t* statics, int32_t* anim, unsigned char* model,
                     char* reason, unsigned capacity);

/* Name2Index 0x4B07C0: the first script of that name, or -1. */
int tagpu_cobvm_index(const TagpuCobProgram* program, const char* name);

/* 0x4B0940 (`argwords` 0: no arguments, sp stays -1) or 0x4B0B00 (`argwords` 1: args[0..3]
   written whatever argc says, sp = argc - 1, and a refusal calls `cb` with 0). `run_now` runs
   every record at dt 0, then the stepper. Returns 1 started, 0 refused, -1 fault. */
int tagpu_cobvm_start(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, int script, uint32_t cb, int run_now,
                      int argwords, int argc, const int32_t args[4]);

/* 0x4B0C40: four words in, sp 3, the thread runs inside the call, the four words come back in
   io. Returns 1, 0 refused (io untouched), -1 fault. */
int tagpu_cobvm_query(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, int script, int32_t io[4]);

/* DoScriptsNow 0x4B0D60: every record with dt, then the stepper. Returns 1, or -1 fault. */
int tagpu_cobvm_tick(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, int32_t dt);

/* Write the latched start's trace line now (the oracle's last line of a run). */
void tagpu_cobvm_flush(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run);

/* The records and the count, read from the object. */
TagpuCobRecord* tagpu_cobvm_record(const TAGPU_COBVM* vm, unsigned slot);
uint32_t        tagpu_cobvm_running(const TAGPU_COBVM* vm);

#endif
