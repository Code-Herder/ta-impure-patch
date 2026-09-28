/* Portable checks shared by the engine adapter and native sanitizer tests. */
#include "tagpu_cob_safe.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char cob_record_size_matches[(sizeof(TagpuCobRecord) == TAGPU_COB_RECORD_BYTES) ? 1 : -1];

int32_t tagpu_cob_build_left(float remaining)
{
    /* DISASSEMBLED: 0x480A44 distinguishes exact completion; 0x480A62
       truncates remaining * -99 before subtracting from one. */
    if (!(remaining >= 0.0f && remaining <= 1.0f)) return -1;
    return remaining == 0.0f ? 0 : 1 - (int32_t)((double)remaining * -99.0);
}

static uint32_t word_at(const unsigned char* p)
{
    uint32_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static int region(size_t size, uint32_t offset, uint32_t count, unsigned stride)
{
    return offset <= size && count <= (size - offset) / stride &&
           (!count || (offset >= 44 && !(offset & 3)));
}

static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return an && bn && a < b + bn && b < a + an;
}

typedef struct CobOp {
    unsigned length, need, put, piece, axis, terminal;
    uint32_t kind;
} CobOp;

static CobOp shape(uint32_t word)
{
    CobOp o = {1, 0, 0, 0, 0, 0, word & 0x100ff000u};
    switch (o.kind) {
    case 0x10001000: case 0x10002000: case 0x10003000:
        o.length = 3; o.need = 2; o.piece = o.axis = 1; break;
    case 0x10004000: case 0x1000b000: case 0x1000c000:
        o.length = 3; o.need = 1; o.piece = o.axis = 1; break;
    case 0x10005000: case 0x10006000: case 0x10007000: case 0x10008000:
    case 0x1000d000: case 0x1000e000:
        o.length = 2; o.piece = 1; break;
    case 0x1000f000: case 0x10071000:
        o.length = 2; o.need = 1; o.piece = 1; break;
    case 0x10011000: case 0x10012000:
        o.length = 3; o.piece = o.axis = 1; break;
    case 0x10013000: case 0x10024000: case 0x10067000: case 0x10068000: case 0x10084000:
        o.need = 1; break;
    case 0x10021000: o.length = 2; o.put = 1; break;
    case 0x10022000: o.put = 1; break;
    case 0x10023000: o.length = 2; o.need = 1; break;
    case 0x10031000: case 0x10032000: case 0x10033000: case 0x10034000:
    case 0x10035000: case 0x10036000: case 0x10037000: case 0x10041000:
    case 0x10051000: case 0x10052000: case 0x10053000: case 0x10054000:
    case 0x10055000: case 0x10056000: case 0x10057000: case 0x10058000: case 0x10059000:
        o.need = 2; o.put = 1; break;
    case 0x10038000: case 0x1005a000: case 0x10042000: case 0x10044000:
        o.need = 1; o.put = 1; break;
    case 0x10045000: o.put = 1; break;
    case 0x10043000: o.need = 5; o.put = 1; break;
    case 0x10061000: case 0x10062000: case 0x10063000: o.length = 3; break;
    case 0x10064000: o.length = 2; break;
    case 0x10065000: break;
    case 0x10066000: o.length = 2; o.need = 1; break;
    case 0x10082000: o.need = 2; break;
    case 0x10083000: o.need = 3; break;
    default: o.terminal = 1; break;
    }
    return o;
}

static const char* operands(const TagpuCobProgram* p, uint32_t pc, CobOp o)
{
    uint32_t a, selector;
    if (pc >= p->words || o.length > p->words - pc) return "instruction extends past code";
    if (o.length == 1) return NULL;
    a = p->code[pc + 1];
    if (o.piece && a >= p->pieces) return "piece index exceeds piece allocation";
    if (o.axis && p->code[pc + 2] >= 3) return "axis is outside X/Y/Z";
    if (o.kind == 0x10021000 || o.kind == 0x10023000) {
        selector = p->code[pc] & 7;
        if (selector == 2 && a >= TAGPU_COB_WORDS) return "local index exceeds stack allocation";
        if (selector == 4 && a >= p->statics) return "static index exceeds static allocation";
        if (selector != 2 && selector != 4 && !(selector == 1 && o.kind == 0x10021000))
            return "unsupported variable selector";
    }
    if (o.kind == 0x10061000 || o.kind == 0x10062000) {
        if (a >= p->scripts) return "child script index exceeds entry table";
        if (p->code[pc + 2] > TAGPU_COB_WORDS) return "child arguments exceed stack allocation";
    }
    if (o.kind == 0x10063000) return "unsupported native-frame call opcode";
    return NULL;
}

static const char* control_flow(TagpuCobProgram* p, uint32_t* bad_pc)
{
    uint32_t *queue, head = 0, tail = 0, i, j;
    const char* error = NULL;
    queue = (uint32_t*)malloc(((size_t)p->words + 1) * sizeof *queue);
    if (!queue) return "COB control-flow allocation failed";
    for (i = 0; i < p->scripts; ++i) {
        uint32_t pc = p->entries[i];
        if (!p->starts[pc]) { queue[tail++] = pc; p->starts[pc] = 4; }
    }
    while (head < tail) {
        uint32_t pc = queue[head++], next[2], n = 0;
        CobOp o;
        *bad_pc = pc;
        if (p->starts[pc] == 2) { error = "branch enters an instruction operand"; break; }
        p->starts[pc] = 1;
        if (pc == p->words) continue;
        o = shape(p->code[pc]);
        if ((error = operands(p, pc, o))) break;
        for (j = 1; j < o.length; ++j) {
            if (p->starts[pc + j] == 1 || p->starts[pc + j] == 4) {
                error = "instruction operand overlaps an entry or branch target";
                break;
            }
            p->starts[pc + j] = 2;
        }
        if (error) break;
        if (o.terminal || o.kind == 0x10065000) continue;
        if (o.kind == 0x10064000 || o.kind == 0x10066000) next[n++] = p->code[pc + 1];
        if (o.kind != 0x10064000) next[n++] = pc + o.length;
        for (j = 0; j < n; ++j) {
            uint32_t target = next[j];
            if (target > p->words || (target == p->words && !p->terminal_zero)) {
                error = "branch or fallthrough leaves code without a terminal zero";
                break;
            }
            if (p->starts[target] == 2) { error = "branch enters an instruction operand"; break; }
            if (!p->starts[target]) { queue[tail++] = target; p->starts[target] = 4; }
        }
        if (error) break;
    }
    free(queue);
    return error;
}

static int fault(char* reason, size_t capacity, const char* text)
{
    if (reason && capacity) snprintf(reason, capacity, "%s", text);
    return 0;
}

int tagpu_cob_check_record(const TagpuCobProgram* p, const TagpuCobRecord* r,
                           char* reason, size_t capacity)
{
    if (r->sp < -1 || r->sp >= (int32_t)TAGPU_COB_WORDS)
        return fault(reason, capacity, "stack pointer exceeds allocation");
    if (!r->status) return 1;
    if (r->pc > p->words || p->starts[r->pc] != 1)
        return fault(reason, capacity, "program counter is not a validated instruction");
    switch (r->status) {
    case 0x01000000: case 0x02400000: break;
    case 0x02100000: case 0x02200000:
        if ((uint32_t)r->piece >= p->pieces || (uint32_t)r->axis >= 3)
            return fault(reason, capacity, "animation wait exceeds piece allocation");
        break;
    case 0x02800000:
        if ((uint32_t)r->child >= TAGPU_COB_THREADS)
            return fault(reason, capacity, "CALL waits on an invalid child slot");
        break;
    default: return fault(reason, capacity, "invalid thread status");
    }
    return 1;
}

int tagpu_cob_check_step(const TagpuCobProgram* p, const TagpuCobRecord* r,
                         char* reason, size_t capacity)
{
    CobOp o;
    unsigned need;
    const char* error;
    if (!tagpu_cob_check_record(p, r, reason, capacity)) return 0;
    if (r->status != 0x01000000)
        return fault(reason, capacity, "attempt to execute a non-running record");
    if (r->pc == p->words) return p->terminal_zero ? 2 : fault(reason, capacity, "code exhausted");
    o = shape(p->code[r->pc]);
    if ((error = operands(p, r->pc, o))) return fault(reason, capacity, error);
    if (o.terminal) return 2;
    need = o.need;
    if (o.kind == 0x10061000 || o.kind == 0x10062000) need = p->code[r->pc + 2];
    if (o.kind == 0x10065000) need = !!r->callback;
    if ((unsigned)(r->sp + 1) < need) return fault(reason, capacity, "operand stack underflow");
    if ((unsigned)(r->sp + 1) - need + o.put > TAGPU_COB_WORDS)
        return fault(reason, capacity, "operand stack capacity exceeded");
    if (o.kind == 0x10034000 && r->stack[r->sp - 1] == INT32_MIN && r->stack[r->sp] == -1)
        return fault(reason, capacity, "integer division overflow");
    return 1;
}

int tagpu_cob_id_valid(int32_t id, uint32_t slots)
{
    return id > 0 && (uint32_t)id < slots && slots <= UINT16_MAX;
}

int tagpu_cob_save_records(const TagpuCobProgram* program, uint32_t checksum,
                           const TagpuCobRecord* records, void* output, size_t size,
                           char* reason, size_t capacity)
{
    uint32_t header[6] = {checksum, 0x53424f43u, 1, TAGPU_COB_WORDS, TAGPU_COB_THREADS, 0};
    TagpuCobRecord normalized[TAGPU_COB_THREADS];
    unsigned i;
    if (size != TAGPU_COB_SAVE_BYTES) return fault(reason, capacity, "wrong COB save buffer size");
    memset(normalized, 0, sizeof normalized);
    for (i = 0; i < TAGPU_COB_THREADS; ++i) {
        if (!records[i].status) continue;
        if (!tagpu_cob_check_record(program, &records[i], reason, capacity)) return 0;
        normalized[i] = records[i];
        /* Native callback objects belong to this process, not the saved game. */
        normalized[i].callback = 0;
        ++header[5];
    }
    memcpy(output, header, sizeof header);
    memcpy((unsigned char*)output + sizeof header, normalized, sizeof normalized);
    return 1;
}

int tagpu_cob_load_records(const TagpuCobProgram* program, uint32_t checksum,
                           const void* input, size_t size, TagpuCobRecord* records,
                           unsigned* busy, char* reason, size_t capacity)
{
    const unsigned char* data = (const unsigned char*)input;
    TagpuCobRecord decoded[TAGPU_COB_THREADS];
    unsigned offset, stride, words, saved_busy, active = 0, i;
    if (size == 0x528u) {
        offset = 4; stride = 164; words = 32;
        saved_busy = word_at(data + size - 4);
    } else if (size == TAGPU_COB_SAVE_BYTES) {
        if (word_at(data + 4) != 0x53424f43u || word_at(data + 8) != 1 ||
            word_at(data + 12) != TAGPU_COB_WORDS || word_at(data + 16) != TAGPU_COB_THREADS)
            return fault(reason, capacity, "unsupported COB save format");
        offset = 24; stride = TAGPU_COB_RECORD_BYTES; words = TAGPU_COB_WORDS;
        saved_busy = word_at(data + 20);
    } else return fault(reason, capacity, "COB saved records are truncated or have an unknown size");
    if (word_at(data) != checksum) return fault(reason, capacity, "saved COB checksum differs from the unit script");
    memset(decoded, 0, sizeof decoded);
    for (i = 0; i < TAGPU_COB_THREADS; ++i) {
        const unsigned char* source = data + offset + i * stride;
        if (!word_at(source)) continue;
        memcpy(&decoded[i], source, stride);
        decoded[i].callback = 0;
        if (decoded[i].sp < -1 || decoded[i].sp >= (int32_t)words)
            return fault(reason, capacity, "saved stack pointer exceeds its original allocation");
        if (!tagpu_cob_check_record(program, &decoded[i], reason, capacity)) return 0;
        ++active;
    }
    if (saved_busy != active) return fault(reason, capacity, "saved active-thread count disagrees with its records");
    /* Do not expose any restored state until every record has passed. */
    memcpy(records, decoded, sizeof decoded);
    *busy = active;
    return 1;
}

void tagpu_cob_program_free(TagpuCobProgram* program)
{
    if (program) {
        free(program->starts);
        free(program->names);
        free(program);
    }
}

TagpuCobProgram* tagpu_cob_program(const void* data, size_t size, char* reason, size_t capacity)
{
    const unsigned char* blob = (const unsigned char*)data;
    uint32_t h[11], offsets[5], lengths[5], i, j;
    TagpuCobProgram* p = NULL;
    const char* error = "COB header is truncated";
    if (reason && capacity) reason[0] = 0;
    if (!blob || size < sizeof h) goto bad;
    error = "COB storage is not word aligned";
    if ((uintptr_t)blob & 3) goto bad;
    error = "COB file exceeds the 16 MiB input capacity";
    if (size > 16u * 1024u * 1024u) goto bad;
    for (i = 0; i < 11; ++i) h[i] = word_at(blob + i * 4);
    error = "COB format is not version 4";
    if (h[0] != 4) goto bad;
    error = "COB count exceeds the supported allocation capacity";
    if (h[1] > 4096 || h[2] > 4096 || h[4] > 4096 || h[5] > 4096) goto bad;
    error = "COB table or code is outside its file allocation";
    if (!region(size, h[6], h[1], 4) || !region(size, h[7], h[1], 4) ||
        !region(size, h[8], h[2], 4) || !region(size, h[9], h[3], 4) ||
        !region(size, h[10], h[5], 8)) goto bad;
    offsets[0] = h[6]; lengths[0] = h[1] * 4;
    offsets[1] = h[7]; lengths[1] = h[1] * 4;
    offsets[2] = h[8]; lengths[2] = h[2] * 4;
    offsets[3] = h[9]; lengths[3] = h[3] * 4;
    offsets[4] = h[10]; lengths[4] = h[5] * 8;
    error = "COB tables overlap code or another table";
    for (i = 0; i < 5; ++i)
        for (j = i + 1; j < 5; ++j)
            if (overlap(offsets[i], lengths[i], offsets[j], lengths[j])) goto bad;
    error = "COB allocation failed";
    p = (TagpuCobProgram*)calloc(1, sizeof *p);
    if (!p) goto bad;
    p->scripts = h[1]; p->pieces = h[2]; p->words = h[3]; p->statics = h[4];
    p->entries = (const uint32_t*)(blob + h[6]);
    p->code = (const uint32_t*)(blob + h[9]);
    p->starts = (unsigned char*)calloc((size_t)p->words + 1, 1);
    p->names = (const char**)calloc(p->scripts ? p->scripts : 1, sizeof *p->names);
    if (!p->starts || !p->names) goto bad;
    /* Some shipped death scripts branch to the first code-table word. Only
       the observed zero terminator is accepted, not arbitrary trailing data. */
    p->terminal_zero = size - h[9] - lengths[3] >= 4 &&
                      word_at(blob + h[9] + lengths[3]) == 0;
    for (i = 0; i < p->scripts + p->pieces + h[5]; ++i) {
        uint32_t offset;
        const unsigned char* end;
        if (i < p->scripts) offset = word_at(blob + h[7] + i * 4);
        else if (i < p->scripts + p->pieces) offset = word_at(blob + h[8] + (i - p->scripts) * 4);
        else offset = word_at(blob + h[10] + (i - p->scripts - p->pieces) * 8 + 4);
        error = "COB name is outside the file or unterminated";
        if (offset < 44 || offset >= size || !(end = memchr(blob + offset, 0, size - offset))) goto bad;
        error = "COB name overlaps a relocated table";
        for (j = 1; j < 5; ++j) {
            if (j == 3) continue;
            if (overlap(offset, (uint32_t)(end - blob - offset) + 1, offsets[j], lengths[j])) goto bad;
        }
        if (i < p->scripts) p->names[i] = (const char*)blob + offset;
    }
    error = "COB script entry is outside code";
    for (i = 0; i < p->scripts; ++i) if (p->entries[i] >= p->words) goto bad;
    if ((error = control_flow(p, &i))) {
        if (reason && capacity) snprintf(reason, capacity, "word %u: %s", i, error);
        tagpu_cob_program_free(p);
        return NULL;
    }
    return p;
bad:
    if (reason && capacity) snprintf(reason, capacity, "%s", error);
    tagpu_cob_program_free(p);
    return NULL;
}
