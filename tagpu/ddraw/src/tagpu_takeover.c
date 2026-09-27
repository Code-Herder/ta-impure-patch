/* tagpu_takeover -- a DirectDraw DLL in the game folder asked for while Impure is the
   process's DirectDraw is answered with Impure itself, so its DllMain never runs.
   research/notes/compat/takeover.md, part 1; the contract is in tagpu_takeover.h. */

#include <windows.h>
#include <string.h>
#include <wchar.h>
#include "dllmain.h"
#include "hook.h"
#include "tagpu_log.h"
#include "tagpu_takeover.h"

/* A DLL file is data until each read out of it has been bounded by the file's own size:
   a mod folder can hold anything, and this runs inside another module's LoadLibrary. */
#define TO_MAX_FILE  (256u << 20)
#define TO_MAX_NAMES 65536u

static const unsigned char* to_at(const unsigned char* b, DWORD size, DWORD off, DWORD n)
{
    return (off <= size && n <= size - off) ? b + off : NULL;
}

/* The file offset of an RVA, through the section table; 0 when no section holds it. */
static DWORD to_rva(const unsigned char* b, DWORD size, const IMAGE_NT_HEADERS32* nt, DWORD rva)
{
    const IMAGE_SECTION_HEADER* sec;
    DWORD first = (DWORD)((const unsigned char*)IMAGE_FIRST_SECTION(nt) - b), i;
    WORD n = nt->FileHeader.NumberOfSections;
    if (!to_at(b, size, first, (DWORD)n * sizeof *sec)) return 0;
    sec = (const IMAGE_SECTION_HEADER*)(b + first);
    for (i = 0; i < n; i++) {
        DWORD span = sec[i].Misc.VirtualSize > sec[i].SizeOfRawData ? sec[i].Misc.VirtualSize
                                                                    : sec[i].SizeOfRawData;
        if (rva >= sec[i].VirtualAddress && rva - sec[i].VirtualAddress < span) {
            DWORD off = rva - sec[i].VirtualAddress;
            if (off >= sec[i].SizeOfRawData) return 0;
            off += sec[i].PointerToRawData;
            return off < size ? off : 0;
        }
    }
    return 0;
}

/* 1 when the 32-bit PE at `b` exports `want` by name. */
static int to_exports(const unsigned char* b, DWORD size, const char* want)
{
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)to_at(b, size, 0, sizeof *dos);
    const IMAGE_NT_HEADERS32* nt;
    const IMAGE_EXPORT_DIRECTORY* ed;
    const DWORD* names;
    DWORD off, i, n, wlen = (DWORD)strlen(want);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return 0;
    nt = (const IMAGE_NT_HEADERS32*)to_at(b, size, (DWORD)dos->e_lfanew, sizeof *nt);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT ||
        !nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress)
        return 0;
    off = to_rva(b, size, nt, nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    ed = off ? (const IMAGE_EXPORT_DIRECTORY*)to_at(b, size, off, sizeof *ed) : NULL;
    if (!ed) return 0;
    n = ed->NumberOfNames < TO_MAX_NAMES ? ed->NumberOfNames : TO_MAX_NAMES;
    off = to_rva(b, size, nt, ed->AddressOfNames);
    names = off ? (const DWORD*)to_at(b, size, off, n * sizeof(DWORD)) : NULL;
    if (!names) return 0;
    for (i = 0; i < n; i++) {
        const char* s;
        off = to_rva(b, size, nt, names[i]);
        s = off ? (const char*)to_at(b, size, off, wlen + 1) : NULL;
        if (s && !memcmp(s, want, wlen + 1)) return 1;
    }
    return 0;
}

static int to_file_exports(const wchar_t* path, const char* want)
{
    HANDLE f, m;
    const unsigned char* b;
    DWORD size;
    int yes = 0;
    /* No FILE_SHARE_WRITE: while this handle is open no handle with write access can exist
       (the open fails if one does), so nothing can shorten the file under the view, and the
       size read here is the size of what is mapped. A file held open for writing is not
       looked at -- the load goes ahead as asked, and the safety net is behind it. */
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(f, NULL);
    if (size != INVALID_FILE_SIZE && size >= sizeof(IMAGE_DOS_HEADER) && size <= TO_MAX_FILE) {
        m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if (m) {
            b = (const unsigned char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            if (b) {
                yes = to_exports(b, size, want);
                UnmapViewOfFile(b);
            }
            CloseHandle(m);
        }
    }
    CloseHandle(f);
    return yes;
}

/* The folder a path is in, with its trailing backslash; 0 when it has none. */
static int to_dir(const wchar_t* path, wchar_t* dir, size_t cap)
{
    const wchar_t* slash = wcsrchr(path, L'\\');
    size_t n;
    if (!slash) return 0;
    n = (size_t)(slash - path) + 1;
    if (n >= cap) return 0;
    wmemcpy(dir, path, n);
    dir[n] = 0;
    return 1;
}

HMODULE tagpu_takeover_loadlibrary_w(const wchar_t* name, DWORD flags, void* caller)
{
    wchar_t exe[MAX_PATH], game[MAX_PATH], self[MAX_PATH], full[MAX_PATH], dir[MAX_PATH], who[MAX_PATH];
    char whoA[MAX_PATH], fullA[MAX_PATH];
    const wchar_t* base;
    wchar_t* file = NULL;
    HMODULE callmod = NULL;
    DWORD n;
    if (!name || !*name || !g_ddraw_module) return NULL;
    if (flags & ~(DWORD)LOAD_WITH_ALTERED_SEARCH_PATH) return NULL;
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH) || !to_dir(exe, game, MAX_PATH)) return NULL;
    if (!GetModuleFileNameW(g_ddraw_module, self, MAX_PATH)) return NULL;
    /* the lever: the suite's proof that the safety net catches what this keeps out */
    if (_snwprintf(full, MAX_PATH, L"%stagpu_takeover.off", game) > 0 &&
        (full[MAX_PATH - 1] = 0, GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES))
        return NULL;

    /* A bare name loads from the application's folder first, which is the game folder; a
       name with a folder in it loads from that folder. Either way, only a file in the
       game folder is a candidate: nothing from the system is ever answered for. */
    if (!wcschr(name, L'\\') && !wcschr(name, L'/')) {
        if (GetModuleHandleW(name)) return NULL;
        n = (DWORD)_snwprintf(full, MAX_PATH, L"%s%s%s", game, name, wcschr(name, L'.') ? L"" : L".dll");
        if (n >= MAX_PATH) return NULL;
        full[MAX_PATH - 1] = 0;
    } else {
        n = GetFullPathNameW(name, MAX_PATH, full, &file);
        if (!n || n >= MAX_PATH) return NULL;
    }
    if (!to_dir(full, dir, MAX_PATH) || _wcsicmp(dir, game)) return NULL;
    if (!_wcsicmp(full, self)) return NULL;
    /* already in the process: its DllMain has run, and answering now would hide nothing */
    if (GetModuleHandleW(full)) return NULL;
    if (!to_file_exports(full, "DirectDrawCreate")) return NULL;

    who[0] = 0;
    if (caller && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCWSTR)caller, &callmod))
        GetModuleFileNameW(callmod, who, MAX_PATH);
    base = wcsrchr(who, L'\\');
    if (!WideCharToMultiByte(CP_ACP, 0, base ? base + 1 : who, -1, whoA, MAX_PATH, NULL, NULL) || !*whoA)
        strcpy(whoA, "?");
    base = wcsrchr(full, L'\\');
    if (!WideCharToMultiByte(CP_ACP, 0, base ? base + 1 : full, -1, fullA, MAX_PATH, NULL, NULL))
        strcpy(fullA, "?");
    tagpu_logf("takeover: %s asked for %s, a DirectDraw DLL in the game folder -- answered "
               "with Impure, so it does not start", whoA, fullA);
    /* A reference like any LoadLibrary's, so a FreeLibrary by the caller balances. */
    return real_LoadLibraryW(self);
}

HMODULE tagpu_takeover_loadlibrary_a(const char* name, DWORD flags, void* caller)
{
    wchar_t w[MAX_PATH];
    if (!name || !MultiByteToWideChar(CP_ACP, 0, name, -1, w, MAX_PATH)) return NULL;
    return tagpu_takeover_loadlibrary_w(w, flags, caller);
}
