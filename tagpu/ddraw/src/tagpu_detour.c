/* tagpu_detour.c — shared stub/patch helpers for the own-the-draw modules.
   Extracted from tagpu_fxown.c when the feature pass needed the same
   flag-gated prologue detour (G13a). See tagpu_detour.h. */

#include <windows.h>
#include <string.h>
#include <stdint.h>
#include "tagpu_detour.h"

unsigned char* tagpu_detour_stub(void)
{
    return (unsigned char*)VirtualAlloc(NULL, 0x80, MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
}

void tagpu_detour_rel(unsigned char* p, unsigned int target)
{
    int32_t rel = (int32_t)(target - ((unsigned int)(size_t)p + 4));
    memcpy(p, &rel, 4);
}

/* 80 3D <flag> 00 */
unsigned char* tagpu_detour_cmp_flag(unsigned char* p, volatile unsigned char* flag)
{
    unsigned int a = (unsigned int)(size_t)flag;
    *p++ = 0x80; *p++ = 0x3D; memcpy(p, &a, 4); p += 4; *p++ = 0x00;
    return p;
}

/* C6 05 <flag> <v> */
unsigned char* tagpu_detour_set_flag(unsigned char* p, volatile unsigned char* flag,
                                     unsigned char v)
{
    unsigned int a = (unsigned int)(size_t)flag;
    *p++ = 0xC6; *p++ = 0x05; memcpy(p, &a, 4); p += 4; *p++ = v;
    return p;
}

int tagpu_detour_write(unsigned int va, const unsigned char* bytes, int n)
{
    DWORD old;
    if (!VirtualProtect((void*)(size_t)va, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy((void*)(size_t)va, bytes, (size_t)n);
    VirtualProtect((void*)(size_t)va, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(size_t)va, (SIZE_T)n);
    return 1;
}

/* Every stub landed on an engine address, so a second module can CHAIN onto a
   site the first one owns instead of overwriting its jmp: the observer
   (tagpu_detour_observe) hooks the earlier stub's copy of the stolen bytes.
   fxown holds CopyGafToContext 0x4B7F90 and the UI census needs to watch it —
   that is the case this exists for (Phase E, G15a). */
typedef struct LANDED { unsigned va; unsigned char* stub; int stolenOff; int nst; } LANDED;
static LANDED s_landed[64];
static int    s_nlanded = 0;

static void detour_record(unsigned int va, unsigned char* stub, int stolenOff, int nst)
{
    if (s_nlanded < (int)(sizeof s_landed / sizeof s_landed[0])) {
        s_landed[s_nlanded].va = va; s_landed[s_nlanded].stub = stub;
        s_landed[s_nlanded].stolenOff = stolenOff; s_landed[s_nlanded].nst = nst;
        s_nlanded++;
    }
}

unsigned char* tagpu_detour_landed(unsigned int va, int* stolenOff, int* nst)
{
    int i;
    for (i = s_nlanded - 1; i >= 0; i--)          /* the newest owner of the site */
        if (s_landed[i].va == va) {
            if (stolenOff) *stolenOff = s_landed[i].stolenOff;
            if (nst) *nst = s_landed[i].nst;
            return s_landed[i].stub;
        }
    return NULL;
}

int tagpu_detour_bytes_ok(unsigned int va, const unsigned char* stolen, int nst)
{
    int off, pn;
    unsigned char* prev = tagpu_detour_landed(va, &off, &pn);
    if (prev) return pn == nst && memcmp(prev + off, stolen, (size_t)nst) == 0;
    return memcmp((const void*)(size_t)va, stolen, (size_t)nst) == 0;
}

/* land the 5-byte jmp on `va` and NOP whatever is left of the stolen bytes */
static int detour_land(unsigned int va, const unsigned char* stub, int nst)
{
    unsigned char jmp5[5];
    unsigned char nops[16];
    jmp5[0] = 0xE9;
    { int32_t rel = (int32_t)((unsigned int)(size_t)stub - (va + 5)); memcpy(jmp5 + 1, &rel, 4); }
    if (!tagpu_detour_write(va, jmp5, 5)) return 0;
    if (nst > 5) {
        memset(nops, 0x90, sizeof nops);
        tagpu_detour_write(va + 5, nops, nst - 5);
    }
    return 1;
}

int tagpu_detour_leaf(unsigned int va, const unsigned char* stolen, int nst,
                      volatile unsigned char* flag, unsigned char retn)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s || nst < 5 || nst > 16) return 0;
    p = tagpu_detour_cmp_flag(p, flag);
    *p++ = 0x74; *p++ = 0x03;                          /* jz +3                */
    *p++ = 0xC2; *p++ = retn; *p++ = 0x00;             /* ret n                */
    detour_record(va, s, (int)(p - s), nst);
    memcpy(p, stolen, (size_t)nst); p += nst;
    *p++ = 0xE9; tagpu_detour_rel(p, va + (unsigned)nst); p += 4;
    return detour_land(va, s, nst);
}

int tagpu_detour_leaf_call(unsigned int va, const unsigned char* stolen, int nst,
                           volatile unsigned char* flag, unsigned char retn,
                           void (__cdecl *fn)(void*))
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    if (!s || !fn || nst < 5 || nst > 16) return 0;
    p = tagpu_detour_cmp_flag(p, flag);
    *p++ = 0x74; *p++ = 0x11;                          /* jz stolen (+17)      */
    /* entry esp E: [E]=retaddr, [E+4]=arg1. pushad leaves esp at E-0x20, so
       arg1 sits at [esp+0x24]; the cdecl call is cleaned by us, and popad
       restores every register (and does not touch flags) before the ret. */
    *p++ = 0x60;                                       /* pushad               */
    *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x24;/* push [esp+0x24]      */
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)fn); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;             /* add esp,4            */
    *p++ = 0x61;                                       /* popad                */
    *p++ = 0xC2; *p++ = retn; *p++ = 0x00;             /* ret n                */
    detour_record(va, s, (int)(p - s), nst);
    memcpy(p, stolen, (size_t)nst); p += nst;
    *p++ = 0xE9; tagpu_detour_rel(p, va + (unsigned)nst); p += 4;
    return detour_land(va, s, nst);
}

int tagpu_detour_observe(unsigned int va, const unsigned char* stolen, int nst,
                         tagpu_detour_before_fn before, tagpu_detour_after_fn after)
{
    unsigned char* s = tagpu_detour_stub();
    unsigned char* p = s;
    unsigned char* tramp;
    unsigned char* prev;
    int prevOff = 0, prevN = 0;
    if (!s || !before || nst < 5 || nst > 16) return 0;
    prev = tagpu_detour_landed(va, &prevOff, &prevN);
    if (prev && (prevN != nst || memcmp(prev + prevOff, stolen, (size_t)nst) != 0)) return 0;
    /* entry: [esp]=retaddr, [esp+4..]=args. pushad puts esp at E-0x20. */
    *p++ = 0x60;                                       /* pushad               */
    *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x20;/* lea eax,[esp+0x20]   */
    *p++ = 0x50;                                       /* push eax  (entry esp)*/
    *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)before); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;             /* add esp,4            */
    *p++ = 0x85; *p++ = 0xC0;                          /* test eax,eax         */
    *p++ = 0x61;                                       /* popad (flags kept)   */
    if (after) {
        *p++ = 0x74; *p++ = 0x07;                      /* jz +7: keep the ret  */
        *p++ = 0xC7; *p++ = 0x04; *p++ = 0x24;         /* mov [esp], imm32     */
        tramp = p + 4 + nst + 5;                       /* the trampoline below */
        { unsigned int t = (unsigned int)(size_t)tramp; memcpy(p, &t, 4); p += 4; }
    }
    detour_record(va, s, (int)(p - s), nst);
    memcpy(p, stolen, (size_t)nst); p += nst;
    *p++ = 0xE9; tagpu_detour_rel(p, va + (unsigned)nst); p += 4;
    if (after) {
        /* the callee has `ret n`-ed here with the caller's esp restored and its
           result in eax. Reserve one slot, save every register, ask `after`
           for the real return address, park it in the slot, restore, `ret`
           into it. No static scratch, so nested and re-entrant use is safe. */
        *p++ = 0x50;                                   /* push eax  (the slot) */
        *p++ = 0x60;                                   /* pushad               */
        *p++ = 0x54;                                   /* push esp  (regs*)    */
        *p++ = 0xE8; tagpu_detour_rel(p, (unsigned int)(size_t)after); p += 4;
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;         /* add esp,4            */
        *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = 0x20; /* mov [esp+0x20],eax */
        *p++ = 0x61;                                   /* popad                */
        *p++ = 0xC3;                                   /* ret -> real return   */
    }
    if (prev) {
        /* chain: the earlier stub's stolen bytes become `jmp s` + NOPs, so its
           non-skip path runs through us and then the original prologue. The
           stub is our own RWX memory: no VirtualProtect, just a flush. */
        unsigned char* q = prev + prevOff;
        int32_t rel = (int32_t)((unsigned int)(size_t)s - ((unsigned int)(size_t)q + 5));
        memset(q, 0x90, (size_t)nst + 5);
        q[0] = 0xE9; memcpy(q + 1, &rel, 4);
        FlushInstructionCache(GetCurrentProcess(), q, (SIZE_T)nst + 5);
        return 1;
    }
    return detour_land(va, s, nst);
}
