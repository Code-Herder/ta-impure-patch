/* tagpu_cobvm.c -- the COB virtual machine, engine-free (tagpu_cobvm.h). Every rule below is the
   retail engine's, DISASSEMBLED from pristine/TotalA.exe.pristine; the addresses are those of
   exe-reverse-engineering.md, "The COB engine -- mapped by us", where each is written up. Where
   Impure's guarded interpreter (tagpu_patches.c, cob_run_step and cob_allocated) differs, the
   IMPURE rules follow it. */
#include "tagpu_cobvm.h"
#include <stdio.h>
#include <string.h>

#define ST_RUN       0x01000000u
#define ST_WAIT_TURN 0x02100000u
#define ST_WAIT_MOVE 0x02200000u
#define ST_SLEEP     0x02400000u
#define ST_CALL      0x02800000u

/* the animation block's fields, three axes each after the flag (0x4B0EC3..0x4B10B3) */
#define F_FLAG        0
#define F_MOVE_TARGET 1
#define F_MOVE_SPEED  4
#define F_TURN_TARGET 7
#define F_TURN_SPEED  10
#define F_SPIN_TARGET 13
#define F_SPIN_ACCEL  16

#define OBJ_RATE    0x04u   /* cob+4: 30, what sleep and the speeds divide by (0x4B0641) */
#define OBJ_DIRTY   0x18u   /* cob+0x18: set by MOVE/TURN/SPIN, cleared by the stepper */
#define OBJ_RECORDS 0x1Cu

/* the model header's words the piece setters write (0x480C50..0x480E2B) */
#define MODEL_BAKE    0x04u  /* zeroed when a cached piece changes */
#define MODEL_CHANGED 0x08u  /* 1 when a position or angle changes */
#define MODEL_COMPOSE 0x10u  /* zeroed by CACHE/DONT_CACHE and SHADE/DONT_SHADE */

/* The longest a record runs without blocking before the VM refuses it. The engine has no
   bound -- such a script hangs the game -- so no script the game runs reaches it. */
#define STEP_BUDGET (1u << 24)

typedef char cobvm_record_at_engine_offset[(OBJ_RECORDS == 28u) ? 1 : -1];
typedef char cobvm_record_size[(sizeof(TagpuCobRecord) == TAGPU_COB_RECORD_BYTES) ? 1 : -1];
typedef char cobvm_record_fields[(offsetof(TagpuCobRecord, pc) == 4 && offsetof(TagpuCobRecord, sp) == 8 &&
                                  offsetof(TagpuCobRecord, sleep) == 0x0C && offsetof(TagpuCobRecord, piece) == 0x10 &&
                                  offsetof(TagpuCobRecord, axis) == 0x14 && offsetof(TagpuCobRecord, child) == 0x18 &&
                                  offsetof(TagpuCobRecord, mask) == 0x1C && offsetof(TagpuCobRecord, callback) == 0x20 &&
                                  offsetof(TagpuCobRecord, stack) == 0x24) ? 1 : -1];
typedef char cobvm_count_after_records[(TAGPU_COB_COUNT_OFFSET == OBJ_RECORDS + TAGPU_COB_THREADS * TAGPU_COB_RECORD_BYTES) ? 1 : -1];

/* ---- the blocks ---- */

static uint32_t obj32(const TAGPU_COBVM* vm, unsigned off)
{
    uint32_t v;
    memcpy(&v, vm->object + off, 4);
    return v;
}

static void obj32_set(TAGPU_COBVM* vm, unsigned off, uint32_t v) { memcpy(vm->object + off, &v, 4); }

TagpuCobRecord* tagpu_cobvm_record(const TAGPU_COBVM* vm, unsigned slot)
{
    return (TagpuCobRecord*)(void*)(vm->object + OBJ_RECORDS + slot * TAGPU_COB_RECORD_BYTES);
}

uint32_t tagpu_cobvm_running(const TAGPU_COBVM* vm) { return obj32(vm, TAGPU_COB_COUNT_OFFSET); }

static unsigned char* prim(const TAGPU_COBVM* vm, int piece)
{
    return vm->model + TAGPU_COBVM_PRIM_BASE + (unsigned)piece * TAGPU_COBVM_PRIM_BYTES;
}

static uint16_t prim16(const unsigned char* p, unsigned off) { uint16_t v; memcpy(&v, p + off, 2); return v; }
static void prim16_set(unsigned char* p, unsigned off, uint16_t v) { memcpy(p + off, &v, 2); }
static void model32_set(TAGPU_COBVM* vm, unsigned off, uint32_t v) { memcpy(vm->model + off, &v, 4); }

/* vt+0x14 0x480C30: a piece's position on an axis */
static int32_t pos_get(const TAGPU_COBVM* vm, int piece, int axis)
{
    int32_t v;
    memcpy(&v, prim(vm, piece) + TAGPU_COBVM_PRIM_POS + axis * 4, 4);
    return v;
}

/* vt+0x18 0x480CB0: a piece's angle on an axis */
static int32_t turn_get(const TAGPU_COBVM* vm, int piece, int axis)
{
    return prim16(prim(vm, piece), TAGPU_COBVM_PRIM_TURN + axis * 2);
}

/* What a pose change does to the model besides the value (0x480C7B..0x480CA7): the piece's stamp
   word cleared, the model marked changed, and a cached piece drops the model's bake. */
static void pose_changed(TAGPU_COBVM* vm, unsigned char* p)
{
    prim16_set(p, TAGPU_COBVM_PRIM_STAMP, 0);
    model32_set(vm, MODEL_CHANGED, 1);
    if (p[TAGPU_COBVM_PRIM_FLAGS] & TAGPU_COBVM_CACHED) model32_set(vm, MODEL_BAKE, 0);
}

/* vt+0x00 0x480C50, only when the value differs */
static void pos_set(TAGPU_COBVM* vm, int piece, int axis, int32_t v)
{
    unsigned char* p = prim(vm, piece);
    if (pos_get(vm, piece, axis) == v) return;
    memcpy(p + TAGPU_COBVM_PRIM_POS + axis * 4, &v, 4);
    pose_changed(vm, p);
}

/* vt+0x04 0x480CE0: the low 16 bits, only when they differ */
static void turn_set(TAGPU_COBVM* vm, int piece, int axis, int32_t v)
{
    unsigned char* p = prim(vm, piece);
    if (prim16(p, TAGPU_COBVM_PRIM_TURN + axis * 2) == (uint16_t)v) return;
    prim16_set(p, TAGPU_COBVM_PRIM_TURN + axis * 2, (uint16_t)v);
    pose_changed(vm, p);
}

/* vt+0x08 0x480D50 (SHOW 1, HIDE 0): bit 0, only when it differs; the model is not marked */
static void visible_set(TAGPU_COBVM* vm, int piece, int on)
{
    unsigned char* p = prim(vm, piece);
    uint16_t f = prim16(p, TAGPU_COBVM_PRIM_FLAGS);
    if ((f & 1u) == (unsigned)on) return;
    prim16_set(p, TAGPU_COBVM_PRIM_FLAGS, (uint16_t)((f & ~1u) | (unsigned)on));
    prim16_set(p, TAGPU_COBVM_PRIM_STAMP, 0);
    if (p[TAGPU_COBVM_PRIM_FLAGS] & TAGPU_COBVM_CACHED) model32_set(vm, MODEL_BAKE, 0);
}

/* vt+0x0C 0x480DB0 (CACHE, bit 1) and vt+0x10 0x480DF0 (SHADE, bit 2): always written, and the
   model's composite dropped */
static void flag_set(TAGPU_COBVM* vm, int piece, unsigned bit, int on)
{
    unsigned char* p = prim(vm, piece);
    uint16_t f = prim16(p, TAGPU_COBVM_PRIM_FLAGS);
    prim16_set(p, TAGPU_COBVM_PRIM_FLAGS, (uint16_t)((f & ~bit) | (on ? bit : 0u)));
    model32_set(vm, MODEL_COMPOSE, 0);
}

/* ---- faults, the trace, the effects ---- */

/* `why` NULL: the guard already wrote the reason into the run */
static int fail(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, const char* why)
{
    vm->faulted = 1;
    if (why && run->reason[0] == 0) snprintf(run->reason, sizeof run->reason, "%s", why);
    return -1;
}

static void trace(TAGPU_COBVM_RUN* run, const TAGPU_COBVM_TRACE* t)
{
    if (run->host && run->host->trace) run->host->trace(run->host->ctx, t);
}

void tagpu_cobvm_flush(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run)
{
    TAGPU_COBVM_TRACE t;
    TagpuCobRecord* r;
    int argc;
    if (!vm->pend.on) return;
    vm->pend.on = 0;
    if (!run->host || !run->host->trace) return;
    r = tagpu_cobvm_record(vm, (unsigned)vm->pend.slot);
    argc = vm->pend.argc < 0 ? r->sp + 1 : vm->pend.argc;
    if (argc < 0) argc = 0;
    if (argc > (int)TAGPU_COB_WORDS) argc = (int)TAGPU_COB_WORDS;
    memset(&t, 0, sizeof t);
    t.kind = 'S';
    t.tick = vm->pend.tick;
    t.slot = vm->pend.slot;
    t.script = vm->pend.script;
    t.pc = -1;
    t.from = vm->pend.from;
    t.parent = vm->pend.parent;
    t.argc = argc;
    t.args = r->stack;
    trace(run, &t);
}

static int effect(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, uint32_t op, int slot, uint32_t pc, int32_t piece,
                  int32_t v0, int32_t v1, int32_t v2)
{
    TAGPU_COBVM_EFFECT e;
    e.op = op; e.slot = slot; e.pc = (int32_t)pc; e.piece = piece;
    e.v[0] = v0; e.v[1] = v1; e.v[2] = v2;
    if (run->host && run->host->commit) { run->host->commit(run->host->ctx, &e); return 1; }
    if (!run->log || run->n >= run->cap) return fail(vm, run, "effect log is full");
    run->log[run->n++] = e;
    return 1;
}

/* ---- the pool ---- */

/* COBEngine_AllocThread 0x4B08C0: -1 for a script the file lacks and for a full pool alike; the
   first free record from 0. The retail engine leaves the rest of the record as its last thread
   left it; Impure's cob_allocated (0x4B0921) clears it. */
static int alloc(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, int script, char from, int parent, int argc)
{
    unsigned slot;
    tagpu_cobvm_flush(vm, run);
    if (script < 0 || (uint32_t)script >= vm->program->scripts) return -1;
    for (slot = 0; slot < vm->threads; ++slot) {
        TagpuCobRecord* r = tagpu_cobvm_record(vm, slot);
        if (r->status) continue;
        if (vm->rules == TAGPU_COBVM_IMPURE) {
            r->sleep = r->piece = r->axis = r->child = 0;
            memset(r->stack, 0, sizeof r->stack);
        }
        r->status = ST_RUN;
        r->pc = vm->program->entries[script];
        r->sp = -1;
        r->callback = 0;
        r->mask = 1;
        obj32_set(vm, TAGPU_COB_COUNT_OFFSET, tagpu_cobvm_running(vm) + 1);
        vm->pend.on = 1;
        vm->pend.slot = (int)slot;
        vm->pend.script = script;
        vm->pend.from = from;
        vm->pend.parent = parent;
        vm->pend.argc = argc;
        vm->pend.tick = run->tick;
        return (int)slot;
    }
    {
        TAGPU_COBVM_TRACE t;
        memset(&t, 0, sizeof t);
        t.kind = 'X'; t.tick = run->tick; t.slot = -1; t.script = script; t.pc = -1; t.from = from;
        t.parent = parent;
        trace(run, &t);
    }
    return -1;
}

static void release(TAGPU_COBVM* vm, unsigned slot)
{
    tagpu_cobvm_record(vm, slot)->status = 0;
    obj32_set(vm, TAGPU_COB_COUNT_OFFSET, tagpu_cobvm_running(vm) - 1);
}

/* every record blocked in a call on `slot` goes runnable (0x4B19FF, 0x4B1AB1) */
static void wake_callers(TAGPU_COBVM* vm, unsigned slot)
{
    unsigned k;
    for (k = 0; k < vm->threads; ++k) {
        TagpuCobRecord* r = tagpu_cobvm_record(vm, k);
        if ((r->status & 0xFFF00000u) == ST_CALL && r->child == (int32_t)slot) r->status = ST_RUN;
    }
}

/* ---- the opcodes ---- */

#define POP()    (r->stack[r->sp--])
#define PUSH(v)  (r->stack[++r->sp] = (v))

static int32_t wrap_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static int32_t wrap_sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static int32_t wrap_mul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }

/* The two-operand words (0x4B1479..0x4B18AB): `b` popped, `a` rewritten in place. NOT and
   BITWISE_NOT neither pop nor push. Division was bounded by tagpu_cob_check_step. */
static void arith(TagpuCobRecord* r, uint32_t op)
{
    int32_t a, b, v;
    if (op == 0x10038000u) { r->stack[r->sp] = ~r->stack[r->sp]; return; }
    if (op == 0x1005A000u) { r->stack[r->sp] = r->stack[r->sp] == 0; return; }
    b = POP();
    a = r->stack[r->sp];
    switch (op) {
    case 0x10031000u: v = wrap_add(a, b); break;
    case 0x10032000u: v = wrap_sub(a, b); break;
    case 0x10033000u: v = wrap_mul(a, b); break;
    case 0x10034000u: v = a / b; break;                  /* MOD's word masks to DIV's */
    case 0x10035000u: v = a & b; break;
    case 0x10036000u: v = a | b; break;
    case 0x10037000u: v = a ^ b; break;
    case 0x10051000u: v = a < b; break;
    case 0x10052000u: v = a <= b; break;
    case 0x10053000u: v = a > b; break;
    case 0x10054000u: v = a >= b; break;
    case 0x10055000u: v = a == b; break;
    case 0x10056000u: v = a != b; break;
    case 0x10057000u: v = a && b; break;
    case 0x10058000u: v = a || b; break;                 /* 0x4B1829 pushes a, 0 there */
    case 0x10059000u: v = a ^ b; break;                  /* 0x4B1875: the bitwise word */
    default:          v = a; break;
    }
    r->stack[r->sp] = v;
}

/* Under IMPURE the extended getters are tagpu_cob_getter's, the rule the game's handler takes
   (tagpu_patches.c, cob_get): an id it answers never reaches the host's own handler, so no host
   can answer one differently. entered() has checked the world is there. */
static int32_t host_get(const TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, int32_t id,
                        int32_t a, int32_t b, int32_t c, int32_t d)
{
    if (vm->rules == TAGPU_COBVM_IMPURE) {
        TagpuCobWorld w = *run->host->world;
        int32_t value;
        w.script_pieces = vm->program->pieces;
        w.model_pieces = vm->program->model_pieces;
        if (tagpu_cob_getter(&w, id, a, &value)) return value;
    }
    return run->host && run->host->get ? run->host->get(run->host->ctx, id, a, b, c, d) : 0;
}

static int exec_thread(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, unsigned slot)
{
    TagpuCobRecord* r = tagpu_cobvm_record(vm, slot);
    const uint32_t* code = vm->program->code;
    int32_t* anim = vm->anim;
    int32_t rate = (int32_t)obj32(vm, OBJ_RATE);
    unsigned steps;
    for (steps = 0; steps < STEP_BUDGET; ++steps) {
        uint32_t pc, word, op, next;
        int checked = tagpu_cob_check_step(vm->program, r, run->reason, sizeof run->reason);
        if (!checked) return fail(vm, run, NULL);
        if (checked == 2) {                 /* 0x4B1B60: an opcode the engine lacks kills its thread */
            release(vm, slot);
            return 1;
        }
        pc = r->pc;
        word = code[pc];
        op = word & 0x100FF000u;
        next = pc + 1;
        switch (op) {
        case 0x10001000u: {                 /* MOVE piece axis: pops target then speed */
            int piece = (int)code[pc + 1], axis = (int)code[pc + 2];
            int32_t* a = anim + piece * TAGPU_COBVM_ANIM;
            int32_t target = POP(), speed;
            a[F_MOVE_TARGET + axis] = target;
            speed = POP() / rate;
            if (target < pos_get(vm, piece, axis)) speed = -speed;
            a[F_MOVE_SPEED + axis] = speed;
            a[F_FLAG] = 1;
            obj32_set(vm, OBJ_DIRTY, 1);
            next = pc + 3;
            break;
        }
        case 0x10002000u: {                 /* TURN piece axis: the short way round */
            int piece = (int)code[pc + 1], axis = (int)code[pc + 2];
            int32_t* a = anim + piece * TAGPU_COBVM_ANIM;
            int32_t target = POP() & 0xFFFF, speed, delta;
            a[F_TURN_TARGET + axis] = target;
            a[F_SPIN_ACCEL + axis] = 0;
            speed = POP() / rate;
            delta = target - turn_get(vm, piece, axis);
            if (delta == 0) speed = 0;
            else if (((delta < 0 ? -delta : delta) > 0x8000) != (delta < 0)) speed = -speed;
            a[F_TURN_SPEED + axis] = speed;
            a[F_FLAG] = 1;
            obj32_set(vm, OBJ_DIRTY, 1);
            next = pc + 3;
            break;
        }
        case 0x10003000u: {                 /* SPIN piece axis: pops speed then acceleration */
            int32_t* a = anim + (int)code[pc + 1] * TAGPU_COBVM_ANIM;
            int axis = (int)code[pc + 2];
            a[F_TURN_TARGET + axis] = -1;   /* no target: the stepper never arrives */
            a[F_SPIN_TARGET + axis] = POP() / rate;
            a[F_SPIN_ACCEL + axis] = POP() / rate;
            if (a[F_SPIN_ACCEL + axis] == 0) a[F_TURN_SPEED + axis] = a[F_SPIN_TARGET + axis];
            a[F_FLAG] = 1;
            obj32_set(vm, OBJ_DIRTY, 1);
            next = pc + 3;
            break;
        }
        case 0x10004000u: {                 /* STOP_SPIN piece axis: pops the deceleration */
            int32_t* a = anim + (int)code[pc + 1] * TAGPU_COBVM_ANIM;
            int axis = (int)code[pc + 2];
            int32_t accel;
            a[F_SPIN_TARGET + axis] = 0;
            accel = -(POP() / rate);
            a[F_SPIN_ACCEL + axis] = accel;
            if (accel == 0) a[F_TURN_SPEED + axis] = 0;
            next = pc + 3;                  /* 0x4B18F8: neither the piece's flag nor cob+0x18 */
            break;
        }
        case 0x10005000u: case 0x10006000u:
            visible_set(vm, (int)code[pc + 1], op == 0x10005000u);
            next = pc + 2;
            break;
        case 0x10007000u: case 0x10008000u:
            flag_set(vm, (int)code[pc + 1], TAGPU_COBVM_CACHED, op == 0x10007000u);
            next = pc + 2;
            break;
        case 0x10009000u:                   /* two pops into vt+0x28 0x480E90, a bare `ret 0xC` */
            r->sp -= 2;
            next = pc + 2;
            break;
        case 0x1000A000u:                   /* vt+0x2C 0x480EA0, a bare `ret 4` */
            next = pc + 2;
            break;
        case 0x1000B000u: {                 /* MOVE_NOW piece axis */
            int piece = (int)code[pc + 1], axis = (int)code[pc + 2];
            int32_t* a = anim + piece * TAGPU_COBVM_ANIM;
            a[F_MOVE_TARGET + axis] = POP();
            a[F_MOVE_SPEED + axis] = 0;
            pos_set(vm, piece, axis, a[F_MOVE_TARGET + axis]);
            next = pc + 3;
            break;
        }
        case 0x1000C000u: {                 /* TURN_NOW piece axis */
            int piece = (int)code[pc + 1], axis = (int)code[pc + 2];
            int32_t* a = anim + piece * TAGPU_COBVM_ANIM;
            a[F_TURN_TARGET + axis] = POP() & 0xFFFF;
            a[F_SPIN_ACCEL + axis] = 0;
            a[F_TURN_SPEED + axis] = 0;
            turn_set(vm, piece, axis, a[F_TURN_TARGET + axis]);
            next = pc + 3;
            break;
        }
        case 0x1000D000u: case 0x1000E000u:
            flag_set(vm, (int)code[pc + 1], TAGPU_COBVM_SHADED, op == 0x1000D000u);
            next = pc + 2;
            break;
        case 0x1000F000u: {                 /* EMIT_SFX piece: pops the type */
            int32_t type = POP();
            if (effect(vm, run, op, (int)slot, pc, (int32_t)code[pc + 1], type, 0, 0) < 0) return -1;
            next = pc + 2;
            break;
        }
        case 0x10011000u: case 0x10012000u: /* WAIT_FOR_TURN / WAIT_FOR_MOVE piece axis */
            r->piece = (int32_t)code[pc + 1];
            r->axis = (int32_t)code[pc + 2];
            r->status = op == 0x10011000u ? ST_WAIT_TURN : ST_WAIT_MOVE;
            r->pc = pc + 3;
            return 1;
        case 0x10013000u:                   /* SLEEP: ms x rate / 1000, truncated */
            r->sleep = wrap_mul(POP(), rate) / 1000;
            r->status = ST_SLEEP;
            r->pc = pc + 1;
            return 1;
        case 0x10021000u: {                 /* PUSH constant / local / static */
            uint32_t sel = word & 7u, at = code[pc + 1];
            int32_t v = sel == 1 ? (int32_t)at : sel == 2 ? r->stack[at] : sel == 4 ? vm->statics[at] : 0;
            PUSH(v);
            next = pc + 2;
            break;
        }
        case 0x10022000u:                   /* CREATE_LOCAL_VAR: the index moves, nothing is written */
            r->sp++;
            break;
        case 0x10023000u: {                 /* POP into a local or a static */
            uint32_t sel = word & 7u, at = code[pc + 1];
            int32_t v = POP();
            if (sel == 2) r->stack[at] = v;
            else if (sel == 4) vm->statics[at] = v;
            next = pc + 2;
            break;
        }
        case 0x10024000u:                   /* POP_STACK */
            r->sp--;
            break;
        case 0x10031000u: case 0x10032000u: case 0x10033000u: case 0x10034000u:
        case 0x10035000u: case 0x10036000u: case 0x10037000u: case 0x10038000u:
        case 0x10051000u: case 0x10052000u: case 0x10053000u: case 0x10054000u:
        case 0x10055000u: case 0x10056000u: case 0x10057000u: case 0x10058000u:
        case 0x10059000u: case 0x1005A000u:
            arith(r, op);
            break;
        case 0x10041000u: {                 /* RAND: pops hi then lo, pushes lo + rng(hi - lo + 1) */
            TAGPU_COBVM_TRACE t;
            int32_t hi = POP(), lo = POP(), n = wrap_add(wrap_sub(hi, lo), 1), got = 0;
            tagpu_cobvm_flush(vm, run);
            if (run->host && run->host->rand) got = run->host->rand(run->host->ctx, n, lo);
            PUSH(wrap_add(lo, got));
            memset(&t, 0, sizeof t);
            t.kind = 'D'; t.tick = run->tick; t.slot = (int32_t)slot; t.script = -1; t.pc = (int32_t)pc;
            t.parent = -1; t.value = r->stack[r->sp]; t.has_value = 1;
            trace(run, &t);
            break;
        }
        case 0x10042000u: {                 /* GET_UNIT_VALUE: pops the id */
            int32_t id = POP();
            PUSH(host_get(vm, run, id, 0, 0, 0, 0));
            break;
        }
        case 0x10043000u: {                 /* GET id a b c d */
            int32_t d = POP(), c = POP(), b = POP(), a = POP(), id = POP();
            PUSH(host_get(vm, run, id, a, b, c, d));
            break;
        }
        case 0x10044000u: {                 /* vt+0x48 0x481430: one pop, the answer pushed */
            int32_t id = POP();
            PUSH(run->host && run->host->cargo ? run->host->cargo(run->host->ctx, id) : 0);
            break;
        }
        case 0x10045000u:                   /* vt+0x4C 0x481470: no pops, the answer pushed */
            PUSH(run->host && run->host->carrier ? run->host->carrier(run->host->ctx) : 0);
            break;
        case 0x10061000u: case 0x10062000u: {   /* START_SCRIPT / CALL_SCRIPT index argc */
            int call = op == 0x10062000u;
            uint32_t argc = code[pc + 2], k;
            int child = alloc(vm, run, (int)code[pc + 1], call ? TAGPU_COBVM_FROM_CALL : TAGPU_COBVM_FROM_START,
                              (int)slot, (int)argc);
            if (child >= 0) {
                TagpuCobRecord* ch = tagpu_cobvm_record(vm, (unsigned)child);
                /* in push order, the child's sp left at -1 (0x4B18EB) */
                for (k = 0; k < argc; ++k) ch->stack[k] = r->stack[r->sp + 1 - (int32_t)argc + (int32_t)k];
                ch->mask = r->mask;
            }
            r->sp -= (int32_t)(vm->rules == TAGPU_COBVM_IMPURE ? tagpu_cob_parent_pops(op, child, argc)
                                                               : (child >= 0 ? argc : 0u));
            if (call) {
                r->child = child;           /* -1 too: nothing wakes that caller (0x4B1965) */
                r->pc = pc + 3;
                r->status = ST_CALL;
                return 1;
            }
            next = pc + 3;
            break;
        }
        case 0x10064000u:                   /* JUMP */
            next = code[pc + 1];
            break;
        case 0x10065000u: {                 /* RETURN */
            TAGPU_COBVM_TRACE t;
            tagpu_cobvm_flush(vm, run);
            memset(&t, 0, sizeof t);
            t.kind = 'R'; t.tick = run->tick; t.slot = (int32_t)slot; t.script = -1; t.pc = (int32_t)pc;
            t.parent = -1;
            if (r->sp >= 0 && r->sp < (int32_t)TAGPU_COB_WORDS) { t.value = r->stack[r->sp]; t.has_value = 1; }
            trace(run, &t);
            if (r->callback) {
                int32_t v = POP();
                if (run->host && run->host->callback) run->host->callback(run->host->ctx, r->callback, v);
            }
            release(vm, slot);
            wake_callers(vm, slot);
            return 1;
        }
        case 0x10066000u:                   /* JUMP_NOT_EQUAL: jumps when the value is zero */
            next = POP() == 0 ? code[pc + 1] : pc + 2;
            break;
        case 0x10067000u: {                 /* SIGNAL: kills every busy record the mask names */
            int32_t mask = POP();
            unsigned k;
            int stop = 0;
            for (k = 0; k < vm->threads; ++k) {
                TagpuCobRecord* o = tagpu_cobvm_record(vm, k);
                TAGPU_COBVM_TRACE t;
                if (!o->status || !(o->mask & (uint32_t)mask)) continue;
                tagpu_cobvm_flush(vm, run);
                memset(&t, 0, sizeof t);
                t.kind = 'K'; t.tick = run->tick; t.slot = (int32_t)k; t.script = -1; t.pc = (int32_t)o->pc;
                t.parent = (int32_t)slot;
                trace(run, &t);
                release(vm, k);             /* its RETURN never runs, so no callback */
                wake_callers(vm, k);
                if (k == slot) stop = 1;
            }
            if (stop) { r->pc = pc + 1; return 1; }
            break;
        }
        case 0x10068000u:                   /* SET_SIGNAL_MASK */
            r->mask = (uint32_t)POP();
            break;
        case 0x10071000u: {                 /* EXPLODE piece: pops the flags */
            int32_t flags = POP();
            if (effect(vm, run, op, (int)slot, pc, (int32_t)code[pc + 1], flags, 0, 0) < 0) return -1;
            next = pc + 2;
            break;
        }
        case 0x10082000u: {                 /* SET id value */
            int32_t value = POP(), id = POP();
            if (effect(vm, run, op, (int)slot, pc, -1, id, value, 0) < 0) return -1;
            break;
        }
        case 0x10083000u: case 0x10084000u: {   /* ATTACH_UNIT (three pops) / DROP_UNIT (one) */
            int words = op == 0x10083000u ? 3 : 1;
            int32_t v[3] = {0, 0, 0};
            int k, live = 1;
            for (k = words - 1; k >= 0; --k) v[k] = POP();
            if (vm->rules == TAGPU_COBVM_IMPURE) {
                if (words == 3 && !tagpu_cob_attach_piece_ok(v[1], vm->program->model_pieces))
                    return fail(vm, run, "attachment piece exceeds its model or wire representation");
                live = run->host && run->host->alive ? run->host->alive(run->host->ctx, v[0]) : 1;
            }
            if (live && effect(vm, run, op, (int)slot, pc, -1, v[0], v[1], v[2]) < 0) return -1;
            break;
        }
        default:                            /* tagpu_cob_check_step answered 2 for every other word */
            return fail(vm, run, "opcode reached the VM unclassified");
        }
        r->pc = next;
    }
    return fail(vm, run, "a thread runs without blocking past the VM's step budget");
}

/* 0x4B0DA0: wake the record if its block has cleared, then run it until it blocks or ends */
static int run_thread(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, unsigned slot, int32_t dt)
{
    TagpuCobRecord* r = tagpu_cobvm_record(vm, slot);
    tagpu_cobvm_flush(vm, run);
    if (!r->status) return 1;
    if (!tagpu_cob_check_record(vm->program, r, run->reason, sizeof run->reason))
        return fail(vm, run, NULL);
    if ((r->status & 0xFF000000u) == 0x02000000u) {
        const int32_t* a = vm->anim + r->piece * (int32_t)TAGPU_COBVM_ANIM;
        switch (r->status) {
        case ST_WAIT_TURN: if (a[F_TURN_SPEED + r->axis] == 0) r->status = ST_RUN; break;
        case ST_WAIT_MOVE: if (a[F_MOVE_SPEED + r->axis] == 0) r->status = ST_RUN; break;
        case ST_SLEEP:
            r->sleep = wrap_sub(r->sleep, dt);
            if (r->sleep <= 0) r->status = ST_RUN;
            break;
        default: break;                     /* a call: only the child's RETURN or a signal wakes it */
        }
    }
    if ((r->status & 0xFF000000u) != ST_RUN) return 1;
    return exec_thread(vm, run, slot);
}

/* 0x4B1C00: returns at once on dt 0 or with nothing animating; else clears each flagged piece's
   flag, advances its three axes, and re-flags it while any still travels. Only a piece below
   the bound model's count is ever flagged (every piece operand was validated against it). */
static void step_anim(TAGPU_COBVM* vm, int32_t dt)
{
    uint32_t piece;
    if (dt == 0 || !obj32(vm, OBJ_DIRTY)) return;
    obj32_set(vm, OBJ_DIRTY, 0);
    for (piece = 0; piece < vm->program->model_pieces; ++piece) {
        int32_t* a = vm->anim + piece * TAGPU_COBVM_ANIM;
        int axis;
        if (!a[F_FLAG]) continue;
        a[F_FLAG] = 0;
        for (axis = 0; axis < 3; ++axis) {
            int32_t speed = a[F_MOVE_SPEED + axis], accel, cur, step, value, target;
            if (speed) {
                target = a[F_MOVE_TARGET + axis];
                value = wrap_add(pos_get(vm, (int)piece, axis), wrap_mul(dt, speed));
                if ((speed > 0 && value >= target) || (speed < 0 && value <= target)) {
                    value = target;
                    a[F_MOVE_SPEED + axis] = 0;
                } else a[F_FLAG] = 1;
                pos_set(vm, (int)piece, axis, value);
            }
            accel = a[F_SPIN_ACCEL + axis];
            if (accel) {
                a[F_TURN_SPEED + axis] = wrap_add(a[F_TURN_SPEED + axis], accel);
                target = a[F_SPIN_TARGET + axis];
                speed = a[F_TURN_SPEED + axis];
                if ((accel > 0 && speed >= target) || (accel < 0 && speed <= target)) {
                    a[F_TURN_SPEED + axis] = target;
                    a[F_SPIN_ACCEL + axis] = 0;
                }
            }
            speed = a[F_TURN_SPEED + axis];
            if (!speed) continue;
            cur = turn_get(vm, (int)piece, axis);
            step = wrap_mul(dt, speed);
            value = wrap_add(cur, step);
            target = a[F_TURN_TARGET + axis];
            if (target == -1) a[F_FLAG] = 1;
            else if (speed > 0) {
                if (((target - cur + 0x10000) & 0xFFFF) > step) a[F_FLAG] = 1;
                else { value = target; a[F_TURN_SPEED + axis] = 0; }
            } else {
                if (((cur - target + 0x10000) & 0xFFFF) > wrap_sub(0, step)) a[F_FLAG] = 1;   /* 0x4B1DAB: neg wraps */
                else { value = target; a[F_TURN_SPEED + axis] = 0; }
            }
            turn_set(vm, (int)piece, axis, value & 0xFFFF);
        }
        if (a[F_FLAG]) obj32_set(vm, OBJ_DIRTY, 1);
    }
}

/* ---- the entries ---- */

int tagpu_cobvm_init(TAGPU_COBVM* vm, const TagpuCobProgram* program, unsigned rules,
                     unsigned char* object, int32_t* statics, int32_t* anim, unsigned char* model,
                     char* reason, unsigned capacity)
{
    memset(vm, 0, sizeof *vm);
    if (!program || !object || !model || (program->statics && !statics) || (program->pieces && !anim) ||
        rules > TAGPU_COBVM_IMPURE || ((uintptr_t)object & 3)) {
        if (reason && capacity) snprintf(reason, capacity, "%s", "COB VM blocks are missing or misaligned");
        return 0;
    }
    vm->program = program;
    vm->object = object;
    vm->statics = statics;
    vm->anim = anim;
    vm->model = model;
    vm->rules = rules;
    vm->threads = rules == TAGPU_COBVM_IMPURE ? TAGPU_COB_THREADS : TAGPU_COB_LEGACY_THREADS;
    memset(object + OBJ_RECORDS, 0, TAGPU_COB_THREADS * TAGPU_COB_RECORD_BYTES + 4u);
    obj32_set(vm, OBJ_RATE, 30);
    obj32_set(vm, OBJ_DIRTY, 0);
    if (program->statics) memset(statics, 0, program->statics * sizeof *statics);
    if (program->pieces) memset(anim, 0, program->pieces * TAGPU_COBVM_ANIM * sizeof *anim);
    return 1;
}

int tagpu_cobvm_index(const TagpuCobProgram* program, const char* name)
{
    uint32_t i;
    for (i = 0; i < program->scripts; ++i)
        if (!strcmp(program->names[i], name)) return (int)i;
    return -1;
}

static int entered(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run)
{
    if (vm->faulted) {
        if (!run->reason[0]) snprintf(run->reason, sizeof run->reason, "%s", "the VM refused this script earlier");
        return 0;
    }
    if ((int32_t)obj32(vm, OBJ_RATE) <= 0) return fail(vm, run, "the object's sim rate is not positive") + 1;
    if (vm->rules == TAGPU_COBVM_IMPURE && !(run->host && run->host->world))
        return fail(vm, run, "an IMPURE host gives no getter world") + 1;
    return 1;
}

int tagpu_cobvm_start(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, int script, uint32_t cb, int run_now,
                      int argwords, int argc, const int32_t args[4])
{
    TagpuCobRecord* r;
    int slot, i;
    if (!entered(vm, run)) return -1;
    if (argwords && (argc < 0 || argc > 4 || !args)) {
        snprintf(run->reason, sizeof run->reason, "%s", "an engine start passes 0 to 4 words");
        return -1;
    }
    slot = alloc(vm, run, script, TAGPU_COBVM_FROM_ENGINE, -1, argwords ? argc : -1);
    if (slot < 0) {
        /* 0x4B0B11: a refused 0x4B0B00 start reports 0 at once; 0x4B0940 calls nothing */
        if (argwords && cb && run->host && run->host->callback) run->host->callback(run->host->ctx, cb, 0);
        return 0;
    }
    r = tagpu_cobvm_record(vm, (unsigned)slot);
    r->callback = cb;
    if (argwords) {
        for (i = 0; i < 4; ++i) r->stack[i] = args[i];
        r->sp = argc - 1;
    }
    if (run_now) {
        unsigned s;
        if (tagpu_cobvm_running(vm))        /* 0x4B0B86 */
            for (s = 0; s < vm->threads; ++s)
                if (run_thread(vm, run, s, 0) < 0) return -1;
        step_anim(vm, 0);
    }
    return 1;
}

int tagpu_cobvm_query(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, int script, int32_t io[4])
{
    TagpuCobRecord* r;
    int slot, i;
    if (!entered(vm, run)) return -1;
    slot = alloc(vm, run, script, TAGPU_COBVM_FROM_ENGINE, -1, -1);
    if (slot < 0) return 0;                 /* the silent failure: io stays untouched */
    r = tagpu_cobvm_record(vm, (unsigned)slot);
    r->callback = 0;
    for (i = 0; i < 4; ++i) r->stack[i] = io[i];
    r->sp = 3;
    if (run_thread(vm, run, (unsigned)slot, 0) < 0) return -1;
    for (i = 0; i < 4; ++i) io[i] = r->stack[i];
    return 1;
}

int tagpu_cobvm_tick(TAGPU_COBVM* vm, TAGPU_COBVM_RUN* run, int32_t dt)
{
    unsigned s;
    if (!entered(vm, run)) return -1;
    for (s = 0; s < vm->threads; ++s)
        if (run_thread(vm, run, s, dt) < 0) return -1;
    step_anim(vm, dt);
    return 1;
}
