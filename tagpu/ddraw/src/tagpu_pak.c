/* tagpu_pak.c -- the embedded archives. Contract: tagpu_pak.h. */
#include <stdint.h>
#include <string.h>
#include "tagpu_pak.h"

static uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

const unsigned char* tagpu_pak_file(const unsigned char* pak, size_t npak, const char* path, size_t* len)
{
    uint32_t count, i, plen = (uint32_t)strlen(path);
    if (!pak || npak < 12 || memcmp(pak, "HPAK", 4) || rd32(pak + 4) != 1) return NULL;
    count = rd32(pak + 8);
    if (count > (npak - 12) / 16) return NULL;
    for (i = 0; i < count; i++) {
        const unsigned char* e = pak + 12 + 16 * i;
        uint32_t no = rd32(e), nl = rd32(e + 4), off = rd32(e + 8), n = rd32(e + 12);
        if (no > npak || nl > npak - no || off > npak || n > npak - off) return NULL;
        if (nl == plen && !memcmp(pak + no, path, plen)) { *len = n; return pak + off; }
    }
    return NULL;
}

#ifdef _WIN32
#include <windows.h>
#include "dllmain.h"

const unsigned char* tagpu_pak_resource(const char* name, size_t* len)
{
    HRSRC r;
    HGLOBAL g;
    const unsigned char* p;
    DWORD n;
    if (!(r = FindResourceA(g_ddraw_module, name, MAKEINTRESOURCEA(10))) || !(g = LoadResource(g_ddraw_module, r))
        || !(p = (const unsigned char*)LockResource(g)) || !(n = SizeofResource(g_ddraw_module, r)))
        return NULL;
    *len = n;
    return p;
}
#endif
