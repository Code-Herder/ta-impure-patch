/* tagpu_cover.c -- the coverage build's bitmap of the functions that ran (inc/tagpu_cover.h).
   Names no engine memory. Only ddraw-cover.dll (TAGPU_COVER) has the bitmap and the hooks the
   instrumented objects call; this file is never instrumented itself, so the hooks cannot recurse. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_cover.h"
#include "tagpu_log.h"

static int cover_name_ok(const char* s)
{
    size_t i, n = strlen(s);
    if (!n || n > 39) return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_'))
            return 0;
    return 1;
}

#ifdef TAGPU_COVER

extern IMAGE_DOS_HEADER __ImageBase;

/* THE BOUND. A bit for each byte of .text, from the DLL's own section header, at most
   COVER_TEXT_MAX bytes of it: an address outside [s_lo, s_hi) marks nothing, so the hook cannot
   write past the bitmap whatever `fn` it is handed. s_lo and s_hi are a function of the image
   alone, so two threads that compute them at once store the same values, and s_ready is set
   after both (InterlockedExchange). */
#define COVER_TEXT_MAX (8u << 20)
static volatile LONG s_bits[COVER_TEXT_MAX / 32];
static volatile DWORD s_lo, s_hi;
static volatile LONG s_ready;

/* the image's base; an empty asm makes it opaque, so the compiler does not take the headers past
   the DOS header for reads outside the one object __ImageBase is declared as */
__attribute__((no_instrument_function)) static const char* cover_base(void)
{
    const char* p = (const char*)&__ImageBase;
    __asm__("" : "+r"(p));
    return p;
}

__attribute__((no_instrument_function)) static void cover_bounds(void)
{
    const char* base = cover_base();
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    unsigned i;
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(sec[i].Name, ".text", 6)) {
            DWORD size = sec[i].Misc.VirtualSize;
            s_lo = sec[i].VirtualAddress;
            s_hi = s_lo + (size < COVER_TEXT_MAX ? size : COVER_TEXT_MAX);
            break;
        }
    InterlockedExchange(&s_ready, 1);
}

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* fn, void* site);
__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void* fn, void* site);

void __cyg_profile_func_enter(void* fn, void* site)
{
    DWORD rva;
    LONG bit;
    (void)site;
    if (!s_ready) cover_bounds();
    rva = (DWORD)((const char*)fn - (const char*)&__ImageBase);
    if (rva < s_lo || rva >= s_hi) return;
    rva -= s_lo;
    bit = (LONG)(1u << (rva & 31));
    if (!(s_bits[rva >> 5] & bit)) InterlockedOr(&s_bits[rva >> 5], bit);
}

void __cyg_profile_func_exit(void* fn, void* site)
{
    (void)fn;
    (void)site;
}

/* The name the first `cover` armed, rewritten by tagpu_cover_flush: set once, before s_armed is
   published (InterlockedCompareExchange claims it, InterlockedExchange publishes it), and never
   changed after, so a flush on another thread reads a whole name or none. */
static char s_name[40];
static volatile LONG s_armed;           /* 0 none, 1 being written, 2 published */

static int cover_dump(const char* name);

int tagpu_cover_write(const char* name)
{
    if (cover_name_ok(name) && InterlockedCompareExchange(&s_armed, 1, 0) == 0) {
        memcpy(s_name, name, strlen(name) + 1);
        InterlockedExchange(&s_armed, 2);
    }
    return cover_dump(name);
}

void tagpu_cover_flush(void)
{
    if (s_armed == 2) cover_dump(s_name);
}

/* written from one copy of the bitmap, so the count and the addresses agree while other threads
   go on marking; one writer at a time (s_writing), since a flush at an exit can meet a trigger's
   write of the same file */
static volatile LONG s_writing;

static int cover_dump_locked(const char* name);

static int cover_dump(const char* name)
{
    int ok;
    while (InterlockedExchange(&s_writing, 1)) Sleep(1);
    ok = cover_dump_locked(name);
    InterlockedExchange(&s_writing, 0);
    return ok;
}

static int cover_dump_locked(const char* name)
{
    char path[80];
    DWORD i, words, n = 0, v;
    LONG* snap;
    FILE* f;
    int ok;
    if (!cover_name_ok(name)) {
        tagpu_logf("cover: %s refused: a name of 1 to 39 letters, digits, - and _", name);
        return 0;
    }
    if (!s_ready) cover_bounds();
    words = (s_hi - s_lo + 31) / 32;
    snap = (LONG*)HeapAlloc(GetProcessHeap(), 0, (words ? words : 1) * sizeof(LONG));
    if (!snap) {
        tagpu_logf("cover: %s not written: no memory for the copy", name);
        return 0;
    }
    for (i = 0; i < words; i++) {
        snap[i] = s_bits[i];
        for (v = (DWORD)snap[i]; v; v &= v - 1) n++;
    }
    CreateDirectoryA("replays", NULL);
    snprintf(path, sizeof path, "replays\\cover-%s.bin", name);
    f = fopen(path, "wb");
    if (!f) {
        HeapFree(GetProcessHeap(), 0, snap);
        tagpu_logf("cover: %s could not be written", path);
        return 0;
    }
    ok = fwrite("ICV1", 1, 4, f) == 4 && fwrite(&n, 4, 1, f) == 1;
    for (i = 0; ok && i < words; i++)
        for (v = (DWORD)snap[i]; ok && v; v &= v - 1) {
            DWORD rva = s_lo + i * 32 + (DWORD)__builtin_ctz(v);
            ok = fwrite(&rva, 4, 1, f) == 1;
        }
    ok = (fclose(f) == 0) && ok;
    HeapFree(GetProcessHeap(), 0, snap);
    tagpu_logf("cover: %s: %lu functions that ran%s", path, (unsigned long)n,
               ok ? "" : ", NOT written whole");
    return ok;
}

#else

void tagpu_cover_flush(void)
{
}

int tagpu_cover_write(const char* name)
{
    tagpu_logf("cover: %s refused: not a coverage build (make COVER=1)",
               cover_name_ok(name) ? name : "?");
    return 0;
}

#endif
