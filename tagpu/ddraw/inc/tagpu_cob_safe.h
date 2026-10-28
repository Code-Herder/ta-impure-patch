#ifndef TAGPU_COB_SAFE_H
#define TAGPU_COB_SAFE_H

#include <stddef.h>
#include <stdint.h>

/* The corpus needs 85 words relative to entry, plus at most four native
   arguments. Every runtime access is checked against this allocation. */
#define TAGPU_COB_WORDS 128u
/* Stock CORACK's reclaim-to-build handoff needs a ninth record: the old
   RequestState/Stop/close chain, MotionControl and its child, SmokeUnit,
   StartBuilding, StopMoving and the new RequestState. All native iterators
   and the object allocation use this same bound. */
#define TAGPU_COB_THREADS 10u
#define TAGPU_COB_LEGACY_THREADS 8u
#define TAGPU_COB_RECORD_BYTES (36u + 4u * TAGPU_COB_WORDS)
#define TAGPU_COB_COUNT_OFFSET (28u + TAGPU_COB_THREADS * TAGPU_COB_RECORD_BYTES)
#define TAGPU_COB_MODEL_OFFSET (TAGPU_COB_COUNT_OFFSET + 4u)
#define TAGPU_COB_OBJECT_BYTES (TAGPU_COB_MODEL_OFFSET + 4u)
#define TAGPU_COB_SAVE_BYTES (24u + TAGPU_COB_THREADS * TAGPU_COB_RECORD_BYTES)
#define TAGPU_COB_LEGACY_SAVE_BYTES (24u + TAGPU_COB_LEGACY_THREADS * TAGPU_COB_RECORD_BYTES)

typedef struct TagpuCobRecord {
    uint32_t status, pc;
    int32_t sp, sleep, piece, axis, child;
    uint32_t mask, callback;
    int32_t stack[TAGPU_COB_WORDS];
} TagpuCobRecord;

typedef struct TagpuCobProgram {
    const uint32_t *code, *entries;
    const char **names;
    unsigned char *starts;
    uint32_t words, scripts, pieces, statics, model_pieces;
    int terminal_zero;
} TagpuCobProgram;

/* The caller owns the blob for the program's entire lifetime. Header/name
   relocation is allowed after validation; code and entries remain immutable. */
TagpuCobProgram* tagpu_cob_program(const void* blob, size_t size, char* reason, size_t capacity);
void tagpu_cob_program_free(TagpuCobProgram* program);
int tagpu_cob_bind_model(TagpuCobProgram* program, uint32_t pieces, char* reason, size_t capacity);
int tagpu_cob_id_valid(int32_t id, uint32_t slots);
int32_t tagpu_cob_build_left(float remaining);
/* 1: execute, 2: stock's terminal-opcode path, 0: fault with a diagnostic. */
int tagpu_cob_check_step(const TagpuCobProgram* program, const TagpuCobRecord* record,
                         char* reason, size_t capacity);
int tagpu_cob_check_record(const TagpuCobProgram* program, const TagpuCobRecord* record,
                           char* reason, size_t capacity);
/* The words a START or CALL takes off the parent's stack. A started child takes its argc; a
   refused CALL keeps them and waits on child -1 (0x4B192F..0x4B1974); a refused START drops
   them, so repeated refused starts cannot fill the parent's stack (the retail engine keeps them). */
uint32_t tagpu_cob_parent_pops(uint32_t kind, int child, uint32_t argc);
/* ATTACH's piece: -1 (hidden cargo, 0x48AC99) or one the model has and the transport packet's
   byte can carry */
int tagpu_cob_attach_piece_ok(int32_t piece, uint32_t model_pieces);

/* THE GETTERS IMPURE ANSWERS: the port-F ids 32 and 69..75 (research/notes/tadr-port/cob.md,
   *Getter contract*) and the bounds on the stock ids 7..11. One rule for the game (tagpu_patches.c
   cob_get, over the engine's units) and for every other host of tagpu_cobvm (over its own world). */
typedef struct TagpuCobUnitFacts {
    uint32_t id, owner, kills;
    float remaining;                /* unit+0x104: the build fraction still to go */
} TagpuCobUnitFacts;
typedef struct TagpuCobWorld {
    void* ctx;
    uint32_t slots;                 /* the unit array's slot count; ids run 1 .. slots - 1 */
    uint32_t script_pieces, model_pieces;
    /* 1 with the facts for a live unit at a valid id, else 0 */
    int (*unit)(void* ctx, int32_t id, TagpuCobUnitFacts* out);
    /* the unit running the script; 0 when it cannot be named */
    int (*caller)(void* ctx, TagpuCobUnitFacts* out);
    int (*allied)(void* ctx, uint32_t own, uint32_t owner);
    /* the owner controlled by a human or an AI on this peer */
    int (*local)(void* ctx, uint32_t owner);
} TagpuCobWorld;
/* 1 with *value for an id this rule answers; 0 for one the stock handler answers */
int tagpu_cob_getter(const TagpuCobWorld* w, int32_t key, int32_t a, int32_t* value);

int tagpu_cob_save_records(const TagpuCobProgram* program, uint32_t checksum,
                           const TagpuCobRecord* records, void* output, size_t size,
                           char* reason, size_t capacity);
int tagpu_cob_load_records(const TagpuCobProgram* program, uint32_t checksum,
                           const void* input, size_t size, TagpuCobRecord* records,
                           unsigned* busy, char* reason, size_t capacity);

#endif
