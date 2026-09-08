/* tagpu_cobtrace.c — the COB script-call oracle (tacob landing 2).

   WHAT IT LOGS. One tab-separated line per event on the game thread, written to
   `tagpu_cobtrace.log` in the game dir (the process cwd) — created afresh at
   every attach (a previous run's file is truncated) and flushed per line, so a
   killed process (`tacli stop`) loses nothing:

     S  tick unit type script slot source args…   a thread starts (allocated)
     R  tick unit slot script value               a thread's RETURN, the value it popped
     X  tick unit script source                   a start REFUSED: all eight records busy
     K  tick unit slot script by                  a thread killed by another's `signal`
     D  tick unit slot value                      a `rand` draw, the value the script got
     #  …                                         comment (the header); parsers skip it

   `tick` is the sim tick `*(main+0x38A47)`, the counter tagpu_posedump.on stamps
   too, so the two join on it. `unit` is the in-game index `*(i16*)(unit+0xA8)`
   (what `tacli roster` prints as idx=), `type` the unit-def name at def+0x20.
   `source` is `E` (the engine called the script through one of its by-name
   entries), `C:<n>` (a script's `start-script`, issued by thread n) or `L:<n>`
   (`call-script` from thread n, which then BLOCKS until the child returns).
   `args` are the words on the child's stack when it first runs, comma-separated,
   in the order the caller pushed them.

   THE SEAM (all disassembly of the pristine build; the engine map has the full
   layouts). The COB engine object hangs off unit+0x9A (0x544 bytes, vtable
   0x4FD698): +0x08 the loaded .cob (+4 nscripts, +0x18 entry[] word indices,
   +0x1C name[] pointers, +0x24 code words), +0x1C eight thread records of
   0xA4 bytes, +0x53C the busy count, +0x540 the posed model whose +0xC is the
   unit. A record: +0 status (0 free; 0x01xxxxxx running; 0x02xxxxxx blocked,
   bits 20-23 say on what), +4 pc, +8 stack top index (-1 empty), +0x18 the
   child slot a `call-script` waits on, +0x1C signal mask, +0x20 the completion
   callback object, +0x24 a 32-word stack.

   Every thread start — the engine's StartScript/QueryScript families and the
   VM's START/CALL opcodes alike — goes through ONE allocator, 0x4B08C0
   (`__thiscall(cob, scriptIndex)` -> record slot 0..7 or -1). Its callers push
   the arguments onto the new record's stack only AFTER it returns, so the S
   line is latched there and written at the next hook event, always before the
   thread has run (the thread runner 0x4B0DA0 is hooked for exactly that: a
   Query* script overwrites its first argument on its first step). The RETURN
   opcode handler at 0x4B19D0 has the value on the stack top and the record in
   esi; the `signal` handler frees a matching record at 0x4B1A99; `rand` calls
   the sim RNG 0x4B6C30 from 0x4B15E0 and pushes lo + result.

   The hooks are byte-matched and installed all-or-nothing at DllMain; nothing
   in the engine is written. Everything runs on the game thread, which is the
   only thread the engine calls its COB VM from, so the log needs no lock. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include "tagpu_detour.h"
#include "tagpu_cobtrace.h"

#define TA_MAINPP   0x00511DE8u
#define OFF_TICK    0x38A47
#define TA()        (*(char**)TA_MAINPP)
#define UDEF(u)     (*(char**)((u) + 0x92))

/* the COB engine object and its records */
#define COB_VTABLE    0x004FD698u          /* the unit script class; a freed block loses it */
#define COB_SIZE      0x544
#define COB_FILE(c)   (*(char**)((c) + 0x08))
#define COB_O3(c)     (*(char**)((c) + 0x540))
#define COB_REC(c, i) ((c) + 0x1C + (i) * 0xA4)
#define REC_PC(r)     (*(int*)((r) + 0x04))
#define REC_SP(r)     (*(int*)((r) + 0x08))
#define REC_STACK(r)  ((int*)((r) + 0x24))
#define REC_WORDS     32
/* the loaded .cob */
#define SF_NSCRIPTS(s) (*(int*)((s) + 0x04))
#define SF_ENTRY(s)    (*(int**)((s) + 0x18))
#define SF_NAMES(s)    (*(char***)((s) + 0x1C))
#define SF_CODE(s)     (*(int**)((s) + 0x24))

/* the five sites and the bytes they hold in the pristine build */
#define ALLOC_VA    0x004B08C0u   /* COBEngine_AllocThread: push esi; mov esi,[esp+8]   */
#define RUN_VA      0x004B0DA0u   /* the thread runner:    sub esp,0x20; push ebx; push ebp */
#define RET_VA      0x004B19D0u   /* RETURN handler:       mov ecx,[esi+0x20]; test ecx,ecx */
#define KILL_VA     0x004B1A99u   /* SIGNAL handler:       mov dword [ecx],0                */
#define RAND_SITE   0x004B15E0u   /* RAND handler:         call 0x4B6C30                    */
#define RAND_FN     0x004B6C30u
#define START_RET   0x004B18C0u   /* AllocThread's return address from the START opcode */
#define CALL_RET    0x004B192Du   /*                               ... from the CALL opcode */
static const unsigned char ALLOC_STOLEN[5] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };
static const unsigned char RUN_STOLEN[5]   = { 0x83, 0xEC, 0x20, 0x53, 0x55 };
static const unsigned char RET_STOLEN[5]   = { 0x8B, 0x4E, 0x20, 0x85, 0xC9 };
static const unsigned char KILL_STOLEN[6]  = { 0xC7, 0x01, 0x00, 0x00, 0x00, 0x00 };
static const unsigned char RAND_STOLEN[5]  = { 0xE8, 0x4B, 0x56, 0x00, 0x00 };

#define FLAG_FILE "tagpu_cobtrace.on"
#define LOG_FILE  "tagpu_cobtrace.log"

typedef int (__thiscall *PFN_Alloc)(void* cob, int idx);

static FILE*     s_f;
static PFN_Alloc s_real_alloc;               /* the stolen prologue + jmp back */
static char      s_filter[256];              /* upper-cased, comma-separated, or "" */
static unsigned  s_skip;                     /* diagnostic: sites left unhooked (bit per site) */
enum { SK_ALLOC = 1, SK_RUN = 2, SK_RET = 4, SK_KILL = 8, SK_RAND = 16 };

/* the latched start: allocated, arguments not yet on the record */
static struct {
    int   valid;
    char* cob;
    int   slot, idx, tick, parent, argc;     /* argc < 0: read sp+1 at flush */
    char  src;                               /* 'E', 'C' or 'L' */
} s_pend;

static void tlog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "cobtrace: %s\n", s); fclose(f); }
}

static int tick(void)
{
    char* ta = TA();
    return ta ? *(int*)(ta + OFF_TICK) : -1;
}

/* the unit behind a COB object: index, and its type name (bounded) */
static char* cob_unit(char* cob)
{
    char* o3 = COB_O3(cob);
    return o3 ? *(char**)(o3 + 0xC) : NULL;
}

static int unit_index(char* u) { return u ? (int)*(short*)(u + 0xA8) : -1; }

static void unit_type(char* u, char* out, int cap)
{
    const char* s = (u && UDEF(u)) ? UDEF(u) + 0x20 : "?";
    int i;
    for (i = 0; i < cap - 1 && s[i] && s[i] != ' '; i++) out[i] = s[i];
    out[i] = 0;
}

/* the type filter from the flag file's contents: "" = everything */
static int wanted(char* u)
{
    char t[40];
    char key[44];
    int  i;
    if (!u) return 0;                 /* no unit behind the object: not a trace line, filter or not */
    if (!s_filter[0]) return 1;
    unit_type(u, t, sizeof t);
    key[0] = ',';
    for (i = 0; t[i] && i < 40; i++) key[i + 1] = (char)toupper((unsigned char)t[i]);
    key[i + 1] = ','; key[i + 2] = 0;
    return strstr(s_filter, key) != NULL;
}

static const char* script_name(char* cob, int idx)
{
    char* sf = COB_FILE(cob);
    if (!sf || idx < 0 || idx >= SF_NSCRIPTS(sf)) return "?";
    return SF_NAMES(sf)[idx];
}

/* the script whose body holds pc: entry points are strictly increasing and
   bodies are contiguous (file-formats.md §2.8), so it is the last entry <= pc */
static const char* script_of_pc(char* cob, int pc)
{
    char* sf = COB_FILE(cob);
    int   i, best = -1, n;
    if (!sf) return "?";
    n = SF_NSCRIPTS(sf);
    for (i = 0; i < n; i++)                       /* the largest entry <= pc; no order assumed */
        if (SF_ENTRY(sf)[i] <= pc && (best < 0 || SF_ENTRY(sf)[i] >= SF_ENTRY(sf)[best])) best = i;
    return best >= 0 ? SF_NAMES(sf)[best] : "?";
}

static void emit(const char* line)
{
    if (!s_f) return;
    fputs(line, s_f);
    fputc('\n', s_f);
    fflush(s_f);      /* per line: the process is killed, never detached, at `tacli stop` */
}

static void flush_pending(void)
{
    char  line[512], t[40], args[REC_WORDS * 12 + 4];
    char* rec;
    char* u;
    int   argc, i, n = 0;
    if (!s_pend.valid) return;
    s_pend.valid = 0;
    /* one event can separate the allocation from this flush, and a unit can
       die inside it: FreeUnitScriptData 0x485E30 frees the 0x544-byte object.
       A freed block keeps its pages (it is above the small-block threshold)
       but loses its vtable word to the heap's free-list links, so that word is
       the check; IsBadReadPtr covers the block being unmapped altogether. */
    if (IsBadReadPtr(s_pend.cob, COB_SIZE) || *(unsigned*)s_pend.cob != COB_VTABLE) {
        emit("# start dropped: its COB object was freed before the next event");
        return;
    }
    /* and the deferral is bounded in time: with any unit alive the runner is
       entered every tick, so a latch older than one tick — or from a previous
       match, whose counter restarted — belongs to an object that is gone */
    if (tick() != s_pend.tick && tick() != s_pend.tick + 1) {
        emit("# start dropped: latched more than one tick ago");
        return;
    }
    rec  = COB_REC(s_pend.cob, s_pend.slot);
    u    = cob_unit(s_pend.cob);
    argc = s_pend.argc >= 0 ? s_pend.argc : REC_SP(rec) + 1;
    if (argc < 0) argc = 0;
    if (argc > REC_WORDS) argc = REC_WORDS;
    args[0] = 0;
    for (i = 0; i < argc; i++)
        n += _snprintf(args + n, sizeof args - (size_t)n, "%s%d", i ? "," : "", REC_STACK(rec)[i]);
    unit_type(u, t, sizeof t);
    if (s_pend.src == 'E')
        _snprintf(line, sizeof line, "S\t%d\t%d\t%s\t%s\t%d\tE\t%s",
                  s_pend.tick, unit_index(u), t, script_name(s_pend.cob, s_pend.idx), s_pend.slot, args);
    else
        _snprintf(line, sizeof line, "S\t%d\t%d\t%s\t%s\t%d\t%c:%d\t%s",
                  s_pend.tick, unit_index(u), t, script_name(s_pend.cob, s_pend.idx), s_pend.slot,
                  s_pend.src, s_pend.parent, args);
    line[sizeof line - 1] = 0;
    emit(line);
}

/* ---- the five hook bodies (game thread) ------------------------------------ */

/* AllocThread wrapper: cob in ecx, idx on the stack; ret is the caller's return
   address, and vm_rec/vm_slot are the VM's esi/ebp — the running record and
   its slot — when the caller is the START or CALL opcode handler. */
static int __stdcall my_alloc(char* cob, int idx, unsigned ret, char* vm_rec, int vm_slot)
{
    int   slot;
    char* u;
    flush_pending();
    slot = s_real_alloc(cob, idx);
    u = cob_unit(cob);
    if (!wanted(u)) return slot;
    if (slot < 0) {
        /* -1 for an index the file lacks too (the engine asking for a script
           the unit does not define); only a valid index is a refused start */
        char* sf = COB_FILE(cob);
        if (sf && idx >= 0 && idx < SF_NSCRIPTS(sf)) {
            char line[200];
            _snprintf(line, sizeof line, "X\t%d\t%d\t%s\t%s", tick(), unit_index(u),
                      script_name(cob, idx), ret == START_RET ? "C" : ret == CALL_RET ? "L" : "E");
            line[sizeof line - 1] = 0;
            emit(line);
        }
        return slot;
    }
    s_pend.valid  = 1;
    s_pend.cob    = cob;
    s_pend.slot   = slot;
    s_pend.idx    = idx;
    s_pend.tick   = tick();
    s_pend.src    = 'E';
    s_pend.parent = -1;
    s_pend.argc   = -1;
    if ((ret == START_RET || ret == CALL_RET) && vm_slot >= 0 && vm_slot < 8 &&
        vm_rec == COB_REC(cob, vm_slot)) {
        /* the opcode's inline operands: [pc+1] script index, [pc+2] argument count */
        char* sf = COB_FILE(cob);
        int   pc = REC_PC(vm_rec);
        s_pend.src    = ret == START_RET ? 'C' : 'L';
        s_pend.parent = vm_slot;
        s_pend.argc   = sf ? SF_CODE(sf)[pc + 2] : -1;
    }
    return slot;
}

/* the thread runner's entry: the latched thread is about to take its first step */
static void __stdcall my_run(char* cob, int slot, int dt)
{
    (void)cob; (void)slot; (void)dt;
    flush_pending();
}

/* RETURN: esi = the record, ebp = its slot, ecx = the pc of the RETURN word */
static void __stdcall my_return(char* cob, char* rec, int slot, int pc)
{
    char  line[200];
    char* u;
    int   sp;
    flush_pending();
    u = cob_unit(cob);
    if (!wanted(u)) return;
    sp = REC_SP(rec);
    if (sp >= 0 && sp < REC_WORDS)
        _snprintf(line, sizeof line, "R\t%d\t%d\t%d\t%s\t%d", tick(), unit_index(u), slot,
                  script_of_pc(cob, pc), REC_STACK(rec)[sp]);
    else
        _snprintf(line, sizeof line, "R\t%d\t%d\t%d\t%s\t?", tick(), unit_index(u), slot,
                  script_of_pc(cob, pc));
    line[sizeof line - 1] = 0;
    emit(line);
}

/* SIGNAL: ecx = the record being freed, ebx = its slot, [esp+0x34] = the signaller */
static void __stdcall my_kill(char* cob, char* rec, int slot, int by)
{
    char  line[200];
    char* u;
    flush_pending();
    u = cob_unit(cob);
    if (!wanted(u)) return;
    _snprintf(line, sizeof line, "K\t%d\t%d\t%d\t%s\t%d", tick(), unit_index(u), slot,
              script_of_pc(cob, REC_PC(rec)), by);
    line[sizeof line - 1] = 0;
    emit(line);
}

/* RAND: ebx = lo, ebp = the slot, r = what 0x4B6C30 returned; the script gets lo + r */
static void __stdcall my_rand(char* cob, int slot, int lo, int r)
{
    char  line[200];
    char* u;
    flush_pending();
    u = cob_unit(cob);
    if (!wanted(u)) return;
    _snprintf(line, sizeof line, "D\t%d\t%d\t%d\t%d", tick(), unit_index(u), slot, lo + r);
    line[sizeof line - 1] = 0;
    emit(line);
}

/* ---- the stubs ------------------------------------------------------------- */

static unsigned char* put_call(unsigned char* p, const void* fn)
{
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)fn); return p + 4;
}
static unsigned char* put_jmp(unsigned char* p, unsigned va)
{
    *p++ = 0xE9; tagpu_detour_rel(p, va); return p + 4;
}

/* AllocThread wrapper. Entry: [esp]=ret [esp+4]=idx, ecx=cob, esi/ebp the caller's.
       push ebp ; push esi                      the VM's record and slot (or junk)
       push [esp+8]                             the return address
       push [esp+0x10]                          idx
       push ecx                                 cob
       call my_alloc                            stdcall(cob, idx, ret, esi, ebp), pops 20
       ret 4                                    exactly what the original does
     real: <5 stolen> ; jmp 0x4B08C5            the trampoline my_alloc calls */
static unsigned char* build_alloc_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x55;                                                /* push ebp          */
    *p++ = 0x56;                                                /* push esi          */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x08;         /* push [esp+8]      */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x10;         /* push [esp+0x10]   */
    *p++ = 0x51;                                                /* push ecx          */
    p = put_call(p, (const void*)&my_alloc);
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;                      /* ret 4             */
    s_real_alloc = (PFN_Alloc)p;
    memcpy(p, ALLOC_STOLEN, 5); p += 5;
    put_jmp(p, ALLOC_VA + 5);
    return s;
}

/* The runner's entry. Entry: [esp]=ret [esp+4]=slot [esp+8]=dt, ecx=cob.
       pushad                                   (+0x20: ret, +0x24 slot, +0x28 dt)
       push [esp+0x28] ; push [esp+0x28]        dt, then slot (the first push moved it)
       push ecx ; call my_run ; popad
       <5 stolen> ; jmp 0x4B0DA5 */
static unsigned char* build_run_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x60;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x28;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x28;
    *p++ = 0x51;
    p = put_call(p, (const void*)&my_run);
    *p++ = 0x61;
    memcpy(p, RUN_STOLEN, 5); p += 5;
    put_jmp(p, RUN_VA + 5);
    return s;
}

/* RETURN, mid-function. esi=record edi=cob ebp=slot ecx=pc.
       pushad ; push ecx ; push ebp ; push esi ; push edi ; call my_return ; popad
       <5 stolen> ; jmp 0x4B19D5                the stolen `test` sets the flags the `je` reads */
static unsigned char* build_ret_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x60;
    *p++ = 0x51; *p++ = 0x55; *p++ = 0x56; *p++ = 0x57;
    p = put_call(p, (const void*)&my_return);
    *p++ = 0x61;
    memcpy(p, RET_STOLEN, 5); p += 5;
    put_jmp(p, RET_VA + 5);
    return s;
}

/* SIGNAL's kill, mid-function. ecx=the record being freed, ebx=its slot, edi=cob,
   [esp+0x34]=the running slot (the runner's first argument; +0x20 under pushad).
       pushad ; push [esp+0x54] ; push ebx ; push ecx ; push edi ; call my_kill ; popad
       <6 stolen> ; jmp 0x4B1A9F */
static unsigned char* build_kill_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0x60;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x54;
    *p++ = 0x53; *p++ = 0x51; *p++ = 0x57;
    p = put_call(p, (const void*)&my_kill);
    *p++ = 0x61;
    memcpy(p, KILL_STOLEN, 6); p += 6;
    put_jmp(p, KILL_VA + 6);
    return s;
}

/* The RNG call site, redirected here. Entry: [esp]=ret [esp+4]=n; ebx=lo ebp=slot edi=cob.
       push [esp+4] ; call 0x4B6C30             the real draw (stdcall, pops n) -> eax
       pushad ; push eax ; push ebx ; push ebp ; push edi ; call my_rand ; popad
       ret 4                                    the callee's own convention */
static unsigned char* build_rand_stub(void)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s) return NULL;
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04;
    p = put_call(p, (const void*)RAND_FN);
    *p++ = 0x60;
    *p++ = 0x50; *p++ = 0x53; *p++ = 0x55; *p++ = 0x57;
    p = put_call(p, (const void*)&my_rand);
    *p++ = 0x61;
    *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;
    return s;
}

/* ---- install --------------------------------------------------------------- */

static void read_filter(void)
{
    char  raw[240];
    FILE* f = fopen(FLAG_FILE, "rb");
    int   n = 0, i, j = 0;
    s_filter[0] = 0;
    if (!f) return;
    n = (int)fread(raw, 1, sizeof raw - 1, f);
    fclose(f);
    if (n <= 0) return;
    raw[n] = 0;
    /* `-alloc -run -ret -kill -rand`: leave that site unhooked (bisection aid;
       the trace is then partial and says so in its header — and with -run the
       S line's arguments may already have been stepped over by the thread) */
    for (i = 0; i + 1 < n; i++) {
        static const struct { const char* w; int len; unsigned bit; } tok[5] = {
            { "alloc", 5, SK_ALLOC }, { "run", 3, SK_RUN }, { "ret", 3, SK_RET },
            { "kill", 4, SK_KILL }, { "rand", 4, SK_RAND } };
        int k, hit = 0;
        if (raw[i] != '-' || (i && (isalnum((unsigned char)raw[i - 1]) || raw[i - 1] == '_'))) continue;
        for (k = 0; k < 5 && !hit; k++) {
            int e = i + 1 + tok[k].len;             /* the word must end there */
            if (e <= n && !_strnicmp(raw + i + 1, tok[k].w, tok[k].len) &&
                !(e < n && (isalnum((unsigned char)raw[e]) || raw[e] == '_'))) {
                s_skip |= tok[k].bit;
                for (; i < e; i++) raw[i] = ' ';     /* strip only a recognised token */
                hit = 1;
            }
        }
    }
    s_filter[j++] = ',';
    for (i = 0; i < n && j < (int)sizeof s_filter - 2; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (isalnum(c) || c == '_') s_filter[j++] = (char)toupper(c);
        else if (s_filter[j - 1] != ',') s_filter[j++] = ',';
    }
    if (s_filter[j - 1] != ',') s_filter[j++] = ',';
    s_filter[j] = 0;
    if (!strcmp(s_filter, ",ALL,") || !strcmp(s_filter, ",")) s_filter[0] = 0;
}

void tagpu_cobtrace_init(void)
{
    unsigned char *sa, *sr, *st, *sk, *sd;
    unsigned char  rel[5];
    char           b[400];
    if (GetFileAttributesA(FLAG_FILE) == INVALID_FILE_ATTRIBUTES) return;
    if (memcmp((void*)ALLOC_VA, ALLOC_STOLEN, 5) != 0 ||
        memcmp((void*)RUN_VA,   RUN_STOLEN,   5) != 0 ||
        memcmp((void*)RET_VA,   RET_STOLEN,   5) != 0 ||
        memcmp((void*)KILL_VA,  KILL_STOLEN,  6) != 0 ||
        memcmp((void*)RAND_SITE, RAND_STOLEN, 5) != 0) {
        tlog("NOT armed — engine bytes differ at one of 0x4B08C0 / 0x4B0DA0 / 0x4B19D0 / 0x4B1A99 / 0x4B15E0");
        return;
    }
    read_filter();
    s_f = fopen(LOG_FILE, "wb");
    if (!s_f) { tlog("NOT armed — cannot create " LOG_FILE); return; }
    fprintf(s_f, "# tagpu_cobtrace v1\tfilter=%s\tcolumns: S tick unit type script slot source args… | "
                 "R tick unit slot script value | X tick unit script source | K tick unit slot script by | "
                 "D tick unit slot value\n", s_filter[0] ? s_filter : "all");
    sa = build_alloc_stub(); sr = build_run_stub(); st = build_ret_stub();
    sk = build_kill_stub();  sd = build_rand_stub();
    if (!sa || !sr || !st || !sk || !sd) { tlog("NOT armed — no stub memory"); fclose(s_f); s_f = NULL; return; }
    /* all five, or none: the first four are 5-byte jmps, the RNG site keeps its E8 */
    /* the call-site redirect: a rel32 is relative to the site it is written
       at, not to this buffer — tagpu_detour_rel() would encode it against the
       stack (and did, once: the first `rand` then called a wild address) */
    {
        int32_t r = (int32_t)((unsigned)(size_t)sd - (RAND_SITE + 5));
        rel[0] = 0xE8; memcpy(rel + 1, &r, 4);
    }
    if (s_skip) {
        char b[120];
        _snprintf(b, sizeof b, "diagnostic: sites skipped%s%s%s%s%s — the trace is partial%s",
                  s_skip & SK_ALLOC ? " alloc" : "", s_skip & SK_RUN ? " run" : "", s_skip & SK_RET ? " ret" : "",
                  s_skip & SK_KILL ? " kill" : "", s_skip & SK_RAND ? " rand" : "",
                  s_skip & SK_RUN ? " (S arguments may be stale without the runner hook)" : "");
        tlog(b);
        fprintf(s_f, "# %s\n", b);
    }
    if (!(s_skip & SK_ALLOC) && !tagpu_detour_land(ALLOC_VA, sa, 5)) { tlog("NOT armed — could not write 0x4B08C0"); fclose(s_f); s_f = NULL; return; }
    if ((!(s_skip & SK_RUN)  && !tagpu_detour_land(RUN_VA, sr, 5)) ||
        (!(s_skip & SK_RET)  && !tagpu_detour_land(RET_VA, st, 5)) ||
        (!(s_skip & SK_KILL) && !tagpu_detour_land(KILL_VA, sk, 6)) ||
        (!(s_skip & SK_RAND) && !tagpu_detour_write(RAND_SITE, rel, 5))) {
        /* put the allocator back so a half-armed image never runs */
        tagpu_detour_write(ALLOC_VA, ALLOC_STOLEN, 5);
        tagpu_detour_write(RUN_VA, RUN_STOLEN, 5);
        tagpu_detour_write(RET_VA, RET_STOLEN, 5);
        tagpu_detour_write(KILL_VA, KILL_STOLEN, 6);
        tlog("NOT armed — a later site refused the write; every site restored");
        fclose(s_f); s_f = NULL;
        return;
    }
    _snprintf(b, sizeof b, "ARMED (" FLAG_FILE " present): 0x4B08C0 0x4B0DA0 0x4B19D0 0x4B1A99 + the call at 0x4B15E0 -> "
              LOG_FILE ", filter=%s", s_filter[0] ? s_filter : "all");
    tlog(b);
}

