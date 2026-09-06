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

/* land the 5-byte jmp on `va` and NOP whatever is left of the stolen bytes */
int tagpu_detour_land(unsigned int va, const unsigned char* stub, int nst)
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
    memcpy(p, stolen, (size_t)nst); p += nst;
    *p++ = 0xE9; tagpu_detour_rel(p, va + (unsigned)nst); p += 4;
    return tagpu_detour_land(va, s, nst);
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
    memcpy(p, stolen, (size_t)nst); p += nst;
    *p++ = 0xE9; tagpu_detour_rel(p, va + (unsigned)nst); p += 4;
    return tagpu_detour_land(va, s, nst);
}
