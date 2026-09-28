#ifndef TAGPU_COB_SAFE_H
#define TAGPU_COB_SAFE_H

#include <stddef.h>
#include <stdint.h>

/* The corpus needs 85 words relative to entry, plus at most four native
   arguments. Every runtime access is checked against this allocation. */
#define TAGPU_COB_WORDS 128u
#define TAGPU_COB_THREADS 8u
#define TAGPU_COB_RECORD_BYTES (36u + 4u * TAGPU_COB_WORDS)
#define TAGPU_COB_COUNT_OFFSET (28u + TAGPU_COB_THREADS * TAGPU_COB_RECORD_BYTES)
#define TAGPU_COB_MODEL_OFFSET (TAGPU_COB_COUNT_OFFSET + 4u)
#define TAGPU_COB_OBJECT_BYTES (TAGPU_COB_MODEL_OFFSET + 4u)
#define TAGPU_COB_SAVE_BYTES (24u + TAGPU_COB_THREADS * TAGPU_COB_RECORD_BYTES)

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
int tagpu_cob_save_records(const TagpuCobProgram* program, uint32_t checksum,
                           const TagpuCobRecord* records, void* output, size_t size,
                           char* reason, size_t capacity);
int tagpu_cob_load_records(const TagpuCobProgram* program, uint32_t checksum,
                           const void* input, size_t size, TagpuCobRecord* records,
                           unsigned* busy, char* reason, size_t capacity);

#endif
