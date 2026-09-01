/* tagpu_suppress.c — G5 render-suppression detour on TA's per-unit draw path.

   Target: DrawUnit 0x45AC20(OFFSCREEN* ctx, UnitStruct* unit) in the pristine
   TotalA.exe (ImageBase 0x400000, md5 8e74a1dffa1f5988624c52048f5b20cd). We hide
   one chosen unit TYPE's *sprite* without touching the simulation: the detour reads
   the unit arg, classifies it read-only, and either returns to the caller at once
   (skipping the whole draw body, observationally pure) or passes through exactly.

   ---------------------------------------------------------------------------
   CALLING CONVENTION (why the suppressed path just does `ret 8`)
   ---------------------------------------------------------------------------
   Disassembly of the pristine exe (objdump -D -b binary -m i386 -M intel
   --adjust-vma=0x400C00, .text VA->file = VA-0x400C00) at both live call sites and
   at DrawUnit's own epilogue proves DrawUnit is STDCALL / callee-cleans-8:

     call site A (0x469A00, the only live one per G4):
       4699fa  lea  eax,[esp+0x34]      ; eax = &offscreen
       4699fe  push esi                 ; push unit   (esi = UnitStruct*)
       4699ff  push eax                 ; push &offscreen
       469a00  call 0x45ac20            ; DrawUnit(&offscreen, unit)
       469a05  mov  ecx,[edi+0x8]       ; <-- NO `add esp,8` : caller does NOT clean

     call site B (0x469BA3, never fires in-game per G4):
       469b9d  lea  ecx,[esp+0x34]
       469ba1  push esi                 ; push unit
       469ba2  push ecx                 ; push &offscreen
       469ba3  call 0x45ac20
       469ba8  mov  ecx,[esp+0x20]      ; <-- NO `add esp,8` : caller does NOT clean

     DrawUnit epilogue:
       45ae7d  c2 08 00                 ; ret 0x8  (callee pops the 8 arg bytes)

   Both sites push (unit, &offscreen) and leave the stack for the callee to clean, and
   DrawUnit ends in `ret 8`. So at the detour entry (E9 lands on the first byte, esp is
   the untouched entry esp): [esp]=retaddr, [esp+4]=&offscreen, [esp+8]=unit. To unwind
   the suppressed call exactly as DrawUnit would, the stub does `ret 8` (0xC2 0x08 0x00):
   pop retaddr into eip and add 8 to esp, removing the two args. DrawUnit's return value
   (eax) is dead — neither caller reads it (both immediately reload ecx) — so no eax set
   is needed. Callee-saved regs (EBX/ESI/EDI/EBP) must be preserved across the call; the
   stub's pushad/popad restores every register to its entry value before the `ret 8`, so
   the suppressed path is indistinguishable from a DrawUnit that drew nothing and returned.

   ---------------------------------------------------------------------------
   THE STUB (runtime-generated into an RWX pool, same technique as tagpu_tracer.c)
   ---------------------------------------------------------------------------
     60                    pushad                       ; save all regs (esp -> E-0x20)
     FF 74 24 28           push [esp+0x28]              ; arg = unit  ([E+8] at this esp)
     E8 <rel32>            call tagpu_suppress_classify ; __cdecl; eax=1 => suppress
     83 C4 04              add  esp,4                   ; pop the cdecl arg
     85 C0                 test eax,eax                 ; set ZF (popad preserves flags)
     61                    popad                        ; restore entry regs (esp -> E)
     75 0C                 jnz  suppress                ; eax!=0 -> skip the draw
     51                    push ecx                 ]
     53                    push ebx                 ]  the 7 stolen prologue bytes
     57                    push edi                 ]  (identical to the tracer passthrough)
     8B 7C 24 14           mov  edi,[esp+0x14]      ]
     E9 <rel32>            jmp  0x45AC27                ; resume DrawUnit body (draw normally)
   suppress:
     C2 08 00              ret  0x8                     ; unwind exactly like DrawUnit

   pushad/popad fully bracket the C classifier, so the classifier may clobber anything;
   the passthrough re-pushes the ORIGINAL ecx/ebx/edi and reads [esp+0x14]=unit exactly
   as the untouched prologue would, then a runtime-computed E9 resumes at 0x45AC27. The
   jnz displacement (0x0C) is the fixed 12-byte length of the passthrough block.

   ---------------------------------------------------------------------------
   UNIT-TYPE CLASSIFICATION (read-only)
   ---------------------------------------------------------------------------
   UnitStruct+0x92 -> UnitDefStruct*. The type's identity strings are inline char
   arrays in UnitDefStruct: Name[0x20]@0x00, UnitName[0x20]@0x20, ObjectName[0x20]@0x80
   (tamem.h). Cross-check: ChallengeResponse.cpp dumps them as CSV columns
   name,unitname,objectname, and TADR's unitshash tool selects "ARMCOM"/"CORCOM" by the
   *unitname* column (== UnitName@0x20). We match the configured token case-insensitively
   against all three identity fields, so "armcom" selects exactly the ARM Commander type
   whichever field carries it; no other type shares that identity string. Nothing here
   writes to the unit — selection, motion, COB animation and the sim are all untouched.

   ---------------------------------------------------------------------------
   COEXISTENCE WITH THE G4 TRACER
   ---------------------------------------------------------------------------
   Both the tracer and this suppressor want the 7 bytes at 0x45AC20. Resolution: when
   "tagpu_suppress.on" is present the suppressor OWNS 0x45AC20, and tagpu_tracer_init()
   skips its DrawUnit detour (it keys off the same trigger file). The tracer's blit hook
   0x459200 is independent and still installs. They never double-detour the same bytes.
*/

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "tagpu_suppress.h"

/* ---- target + engine layout (all G3/G4/tamem.h-verified) --------------------------- */
#define DRAWUNIT_VA      0x0045AC20u   /* DrawUnit entry                                */
#define DRAWUNIT_RESUME  0x0045AC27u   /* clean boundary after the 7 stolen bytes       */

#define U_UNITTYPE       0x92          /* UnitStruct+0x92 -> UnitDefStruct*             */
#define UD_NAME          0x00          /* UnitDefStruct.Name[0x20]       (CSV "name")    */
#define UD_UNITNAME      0x20          /* UnitDefStruct.UnitName[0x20]   (CSV "unitname")*/
#define UD_OBJNAME       0x80          /* UnitDefStruct.ObjectName[0x20] (CSV "objectname") */
#define NAME_FIELD_LEN   0x20          /* inline field width                            */

/* The 7 stolen prologue bytes (byte-match guard, exactly like tagpu_tracer.c):
   51 53 57 8B 7C 24 14 = push ecx; push ebx; push edi; mov edi,[esp+0x14]. */
static const unsigned char DU_STOLEN[7] = { 0x51, 0x53, 0x57, 0x8B, 0x7C, 0x24, 0x14 };

/* ---- state ------------------------------------------------------------------------- */
static int               g_armed        = 0;
static char              g_target[32]   = "armcom";   /* lowercased type token          */
static unsigned char*    g_stub         = 0;

static volatile unsigned g_hits         = 0;          /* suppressed calls this window   */
static volatile unsigned g_passed       = 0;          /* passed-through calls this window */
static unsigned          g_hits_total   = 0;
static unsigned          g_passed_total = 0;
static unsigned          g_last_summary = 0;

static void slog(const char* s)
{
    FILE* f = fopen("tagpu.log", "a");
    if (f) { fprintf(f, "%s\n", s); fclose(f); }
}

/* sim/heap pointers live well above 0x600000 (matches overlay/tracer guards) */
static int ptr_ok(unsigned int p) { return p > 0x00600000u && p < 0x7FFF0000u; }

/* ---- classification (read-only, hot path; no CRT file I/O) ------------------------- */

/* Case-insensitive EXACT match of one inline name field (<=0x20 bytes) against
   g_target (already lowercased, NUL-terminated). Exact so "armcom" never matches
   "armcomdead" etc. field is guaranteed readable for NAME_FIELD_LEN bytes. */
static int name_matches(const char* field)
{
    int i;
    for (i = 0; i < NAME_FIELD_LEN; i++) {
        unsigned char a = (unsigned char)field[i];
        unsigned char b = (unsigned char)g_target[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (a != b) return 0;
        if (b == 0) return 1;          /* both terminated at the same index */
    }
    return 1;                          /* full 0x20-char match (target uses full width) */
}

static int should_suppress(unsigned int unit)
{
    unsigned int def;
    if (!ptr_ok(unit)) return 0;
    def = *(volatile unsigned int*)(unit + U_UNITTYPE);
    if (!ptr_ok(def)) return 0;
    if (name_matches((const char*)(def + UD_NAME)))     return 1;
    if (name_matches((const char*)(def + UD_UNITNAME))) return 1;
    if (name_matches((const char*)(def + UD_OBJNAME)))  return 1;
    return 0;
}

int __cdecl tagpu_suppress_classify(unsigned int unit)
{
    if (should_suppress(unit)) { g_hits++;   return 1; }
    g_passed++;
    return 0;
}

/* ---- install: byte-match guard, generate stub, write E9 ---------------------------- */
static int install(void)
{
    unsigned char* t = (unsigned char*)DRAWUNIT_VA;
    unsigned char* s;
    unsigned char* p;
    DWORD old;
    int32_t rel;

    if (memcmp(t, DU_STOLEN, 7) != 0) return 0;     /* wrong build — leave untouched */

    s = (unsigned char*)VirtualAlloc(NULL, 0x100,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!s) return 0;
    g_stub = s;
    p = s;

    *p++ = 0x60;                                              /* pushad               */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x28;       /* push [esp+0x28] unit */
    *p++ = 0xE8;                                              /* call rel32 classify  */
    rel = (int32_t)((unsigned int)&tagpu_suppress_classify - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                    /* add esp,4            */
    *p++ = 0x85; *p++ = 0xC0;                                 /* test eax,eax         */
    *p++ = 0x61;                                              /* popad                */
    *p++ = 0x75; *p++ = 0x0C;                                 /* jnz +12 -> suppress  */
    /* passthrough (12 bytes): the 7 stolen bytes, then jmp to resume */
    *p++ = 0x51;                                              /* push ecx             */
    *p++ = 0x53;                                              /* push ebx             */
    *p++ = 0x57;                                              /* push edi             */
    *p++ = 0x8B; *p++ = 0x7C; *p++ = 0x24; *p++ = 0x14;       /* mov edi,[esp+0x14]   */
    *p++ = 0xE9;                                              /* jmp rel32 resume     */
    rel = (int32_t)(DRAWUNIT_RESUME - ((unsigned int)p + 4));
    memcpy(p, &rel, 4); p += 4;
    /* suppress path */
    *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;                    /* ret 8                */

    if (!VirtualProtect(t, 7, PAGE_EXECUTE_READWRITE, &old)) return 0;
    t[0] = 0xE9;                                              /* jmp rel32 -> stub    */
    rel = (int32_t)((unsigned int)s - (DRAWUNIT_VA + 5));
    memcpy(t + 1, &rel, 4);
    t[5] = 0x90; t[6] = 0x90;                                 /* NOP-fill 2 leftover  */
    VirtualProtect(t, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 7);
    return 1;
}

/* ---- read the suppress target from the trigger file (first token, lowercased) ------ */
static void read_target(void)
{
    HANDLE h;
    DWORD  n = 0;
    char   buf[64];
    int    i, j;

    h = CreateFileA("tagpu_suppress.on", GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;                    /* keep default "armcom" */

    if (ReadFile(h, buf, (DWORD)(sizeof(buf) - 1), &n, NULL) && n > 0) {
        buf[n] = 0;
        i = 0;
        while (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n') i++;
        j = 0;
        while (buf[i] && j < 31 &&
               buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n') {
            char c = buf[i++];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            g_target[j++] = c;
        }
        if (j > 0) g_target[j] = 0;                           /* else keep default     */
    }
    CloseHandle(h);
}

/* ---- init: arm only if the trigger file exists ------------------------------------- */
void tagpu_suppress_init(void)
{
    char b[160];

    if (GetFileAttributesA("tagpu_suppress.on") == INVALID_FILE_ATTRIBUTES) return; /* off */

    read_target();
    g_armed = install();

    _snprintf(b, sizeof b,
        "suppress: %s (tagpu_suppress.on present). DrawUnit@0x45AC20 hook=%s target=\"%s\" stub=%p",
        g_armed ? "ARMED" : "not armed",
        g_armed ? "OK" : "SKIP(byte-mismatch)", g_target, (void*)g_stub);
    slog(b);
}

/* ---- flush: per-frame, from the overlay callback (no-op unless armed) --------------- */
void tagpu_suppress_flush(unsigned int frame_counter)
{
    if (!g_armed) return;

    if (frame_counter - g_last_summary >= 60) {
        unsigned h  = g_hits;
        unsigned pa = g_passed;
        char b[160];

        g_hits = 0; g_passed = 0;
        g_hits_total   += h;
        g_passed_total += pa;

        _snprintf(b, sizeof b,
            "SUPP type=%s hits=%u passed=%u (total hits=%u passed=%u)",
            g_target, h, pa, g_hits_total, g_passed_total);
        slog(b);

        g_last_summary = frame_counter;
    }
}
