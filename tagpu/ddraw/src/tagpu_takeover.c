/* tagpu_takeover -- a DirectDraw DLL in the game folder asked for while Impure is the
   process's DirectDraw is answered with Impure itself, so its DllMain never runs.
   research/notes/compat/takeover.md, part 1; the contract is in tagpu_takeover.h. */

#include <windows.h>
#include <stdio.h>
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

/* The lever: the suite's harness setups, which show the safety net and the recorder check
   catching what this keeps out. */
static int to_off(const wchar_t* game)
{
    wchar_t path[MAX_PATH];
    if (_snwprintf(path, MAX_PATH, L"%stagpu_takeover.off", game) <= 0) return 0;
    path[MAX_PATH - 1] = 0;
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
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
    if (to_off(game)) return NULL;

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

/* ---- the recorder: the exe's DirectPlay imports (tagpu_takeover.h) ---------------------- */

/* Windows' own DirectPlay, loaded by the first forwarder called. Published once with a
   compare-exchange: two threads that both load it hold one module twice, and the loser
   drops its reference. Never freed: the process keeps it. */
static HMODULE volatile s_dp_sys;

static FARPROC dp_proc(const char* name)
{
    HMODULE m = s_dp_sys;
    if (!m) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        HMODULE mine;
        if (!n || n >= MAX_PATH - 12) return NULL;
        wcscpy(path + n, L"\\dplayx.dll");
        mine = LoadLibraryW(path);
        if (!mine) {
            tagpu_logf("takeover: Windows' dplayx.dll could not be loaded (error %lu)",
                       GetLastError());
            return NULL;
        }
        m = (HMODULE)InterlockedCompareExchangePointer((PVOID volatile*)&s_dp_sys, mine, NULL);
        if (m) FreeLibrary(mine);
        else {
            m = mine;
            tagpu_log("takeover: the game's DirectPlay is Windows' dplayx.dll");
        }
    }
    return GetProcAddress(m, name);
}

/* The DirectPlay exports an exe imports, by the ordinals Microsoft's dplayx.dll gives them
   -- the numbers TotalA.exe imports DPLAYX.dll by, which every DirectPlay stand-in exports
   under too, or the exe would not bind to it. All stdcall; E_FAIL when Windows has no
   DirectPlay, as a system without it answers. */
typedef HRESULT (WINAPI* dp_create_fn)(void*, void*, void*);
typedef HRESULT (WINAPI* dp_enum_fn)(void*, void*);
typedef HRESULT (WINAPI* dp_lobby_fn)(void*, void*, void*, void*, DWORD);

static HRESULT WINAPI dp_create(void* guid, void* out, void* outer)
{
    dp_create_fn f = (dp_create_fn)dp_proc("DirectPlayCreate");
    return f ? f(guid, out, outer) : E_FAIL;
}
static HRESULT WINAPI dp_enum_a(void* cb, void* ctx)
{
    dp_enum_fn f = (dp_enum_fn)dp_proc("DirectPlayEnumerateA");
    return f ? f(cb, ctx) : E_FAIL;
}
static HRESULT WINAPI dp_enum_w(void* cb, void* ctx)
{
    dp_enum_fn f = (dp_enum_fn)dp_proc("DirectPlayEnumerateW");
    return f ? f(cb, ctx) : E_FAIL;
}
static HRESULT WINAPI dp_enum(void* cb, void* ctx)
{
    dp_enum_fn f = (dp_enum_fn)dp_proc("DirectPlayEnumerate");
    return f ? f(cb, ctx) : E_FAIL;
}
static HRESULT WINAPI dp_lobby_a(void* sp, void* out, void* outer, void* data, DWORD size)
{
    dp_lobby_fn f = (dp_lobby_fn)dp_proc("DirectPlayLobbyCreateA");
    return f ? f(sp, out, outer, data, size) : E_FAIL;
}
static HRESULT WINAPI dp_lobby_w(void* sp, void* out, void* outer, void* data, DWORD size)
{
    dp_lobby_fn f = (dp_lobby_fn)dp_proc("DirectPlayLobbyCreateW");
    return f ? f(sp, out, outer, data, size) : E_FAIL;
}

static const struct { WORD ordinal; const char* name; FARPROC fn; } s_dp[] = {
    { 1, "DirectPlayCreate",       (FARPROC)dp_create  },
    { 2, "DirectPlayEnumerateA",   (FARPROC)dp_enum_a  },
    { 3, "DirectPlayEnumerateW",   (FARPROC)dp_enum_w  },
    { 4, "DirectPlayLobbyCreateA", (FARPROC)dp_lobby_a },
    { 5, "DirectPlayLobbyCreateW", (FARPROC)dp_lobby_w },
    { 9, "DirectPlayEnumerate",    (FARPROC)dp_enum    },
};
#define DP_N (sizeof s_dp / sizeof s_dp[0])

/* The forwarder for one import: by ordinal or by name; NULL for anything else the exe could
   import from a DirectPlay DLL (gdwDPlaySPRefCount is data, the COM entry points are not an
   exe's), which leaves the whole descriptor alone. */
static FARPROC dp_forwarder(const BYTE* exe, DWORD image, DWORD thunk)
{
    size_t i;
    if (IMAGE_SNAP_BY_ORDINAL32(thunk)) {
        for (i = 0; i < DP_N; i++)
            if (s_dp[i].ordinal == IMAGE_ORDINAL32(thunk)) return s_dp[i].fn;
        return NULL;
    }
    if (thunk >= image || image - thunk < sizeof(IMAGE_IMPORT_BY_NAME) + 24) return NULL;
    for (i = 0; i < DP_N; i++)
        if (!strncmp((const char*)((const IMAGE_IMPORT_BY_NAME*)(exe + thunk))->Name,
                     s_dp[i].name, 24))
            return s_dp[i].fn;
    return NULL;
}

/* The module a bound slot leads into, when it is a DirectPlay DLL of the game folder that
   is not Impure: its file name (for the log) in `who`, else 0. */
static int dp_foreign(const void* target, const wchar_t* game, char* who, size_t cap)
{
    HMODULE m = NULL;
    wchar_t path[MAX_PATH], dir[MAX_PATH];
    const wchar_t* base;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &m) ||
        m == (HMODULE)g_ddraw_module || !GetModuleFileNameW(m, path, MAX_PATH) ||
        !to_dir(path, dir, MAX_PATH) || _wcsicmp(dir, game) ||
        !GetProcAddress(m, "DirectPlayCreate"))
        return 0;
    base = wcsrchr(path, L'\\');
    if (!WideCharToMultiByte(CP_ACP, 0, base ? base + 1 : path, -1, who, (int)cap, NULL, NULL))
        strcpy(who, "?");
    return 1;
}

void tagpu_takeover_dplay_init(void)
{
    BYTE* exe = (BYTE*)GetModuleHandleW(NULL);
    wchar_t path[MAX_PATH], game[MAX_PATH];
    const IMAGE_NT_HEADERS32* nt;
    const IMAGE_DATA_DIRECTORY* dd;
    DWORD image, at, end;

    if (!exe || !g_ddraw_module) return;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !to_dir(path, game, MAX_PATH)) return;
    nt = (const IMAGE_NT_HEADERS32*)(exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
    image = nt->OptionalHeader.SizeOfImage;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || dd->VirtualAddress >= image) return;
    end = image - (DWORD)sizeof(IMAGE_IMPORT_DESCRIPTOR);

    /* Every RVA below is the exe's own, bounded by its image before it is followed. */
    for (at = dd->VirtualAddress; at <= end; at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const IMAGE_IMPORT_DESCRIPTOR* d = (const IMAGE_IMPORT_DESCRIPTOR*)(exe + at);
        DWORD names, slots, k, n = 0;
        char who[MAX_PATH], dll[64];
        if (!d->FirstThunk) break;
        names = d->OriginalFirstThunk;
        slots = d->FirstThunk;
        if (slots >= image || d->Name >= image) continue;
        /* the descriptor's DLL, for the log: bounded copy of a name inside the image */
        _snprintf(dll, sizeof dll, "%.*s", (int)(image - d->Name < 63 ? image - d->Name : 63),
                  (const char*)(exe + d->Name));
        dll[sizeof dll - 1] = 0;
        if (slots + sizeof(DWORD) > image ||
            !dp_foreign((const void*)(size_t)*(const DWORD*)(exe + slots), game, who, sizeof who))
            continue;
        /* the bound slots hold addresses: what each one imports is only in the name table */
        if (!names || names >= image) {
            tagpu_logf("takeover: %s's slots lead into %s, but the exe carries no name table "
                       "for them -- the recorder is left running", dll, who);
            continue;
        }
        if (to_off(game)) {
            tagpu_logf("takeover: %s's slots lead into %s, TADR's recorder -- left, "
                       "tagpu_takeover.off", dll, who);
            return;
        }
        /* all or nothing: every slot of the descriptor named, and in bounds, first */
        for (k = 0;; k++) {
            DWORD t;
            if (names + (k + 1) * sizeof(DWORD) > image || slots + (k + 1) * sizeof(DWORD) > image) {
                n = 0;
                break;
            }
            t = ((const DWORD*)(exe + names))[k];
            if (!t) break;
            if (!dp_forwarder(exe, image, t)) {
                tagpu_logf("takeover: %s's import %lu leads into %s and is not a DirectPlay "
                           "entry Impure forwards -- the recorder is left running",
                           dll, k, who);
                n = 0;
                break;
            }
            n++;
        }
        if (!n) continue;
        {
            DWORD* slot = (DWORD*)(exe + slots), old;
            if (!VirtualProtect(slot, n * sizeof(DWORD), PAGE_READWRITE, &old)) {
                tagpu_logf("takeover: %s's import slots could not be written (error %lu) -- "
                           "the recorder is left running", dll, GetLastError());
                continue;
            }
            for (k = 0; k < n; k++)
                slot[k] = (DWORD)(size_t)dp_forwarder(exe, image, ((const DWORD*)(exe + names))[k]);
            VirtualProtect(slot, n * sizeof(DWORD), old, &old);
        }
        tagpu_logf("takeover: %s's %lu DirectPlay imports led into %s, a DirectPlay DLL in the "
                   "game folder -- pointed at Windows' own, so it does not start", dll, n, who);
    }
}
