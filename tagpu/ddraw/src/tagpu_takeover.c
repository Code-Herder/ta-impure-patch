/* tagpu_takeover -- TADR's DLLs run none of their own code, and no launch goes ahead with a
   byte of the game's code leading into one. research/notes/compat/takeover.md, part 1; the
   four passes and what each rests on are in tagpu_takeover.h. */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "dllmain.h"
#include "hook.h"
#include "utils.h"
#include "tagpu_log.h"
#include "tagpu_refuse.h"
#include "tagpu_takeover.h"
#include "git.h"

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

/* Maps `path` read-only and answers ask(bytes, size, ctx); 0 when the file cannot be read.

   No FILE_SHARE_WRITE: while this handle is open no handle with write access can exist
   (the open fails if one does), so nothing can shorten the file under the view, and the
   size read here is the size of what is mapped. A file held open for writing is not
   looked at -- the load goes ahead as asked, and the safety net is behind it. */
static int to_file_ask(const wchar_t* path,
                       int (*ask)(const unsigned char*, DWORD, void*), void* ctx)
{
    HANDLE f, m;
    const unsigned char* b;
    DWORD size;
    int yes = 0;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(f, NULL);
    if (size != INVALID_FILE_SIZE && size >= sizeof(IMAGE_DOS_HEADER) && size <= TO_MAX_FILE) {
        m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if (m) {
            b = (const unsigned char*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            if (b) {
                yes = ask(b, size, ctx);
                UnmapViewOfFile(b);
            }
            CloseHandle(m);
        }
    }
    CloseHandle(f);
    return yes;
}

static int to_ask_exports(const unsigned char* b, DWORD size, void* ctx)
{
    return to_exports(b, size, (const char*)ctx);
}

static int to_file_exports(const wchar_t* path, const char* want)
{
    return to_file_ask(path, to_ask_exports, (void*)want);
}

/* TADR's own marker strings, looked for in a FILE of the game folder -- never its name, which
   the mods change. MEASURED 2026-09-27 over the suite's 130 fixture files: TO_TADR is in all
   18 TADR modules and in nothing else; TO_REC is in the 9 recorders (tagpu_takeover.h). */
#define TO_TADR "TADemo-MKChat"
#define TO_REC  "TA Demo Recorder"

static int to_find(const unsigned char* b, DWORD size, const char* s)
{
    DWORD n = (DWORD)strlen(s), i;
    if (size < n) return 0;
    for (i = 0; i + n <= size; i++)
        if (b[i] == (unsigned char)s[0] && !memcmp(b + i, s, n)) return 1;
    return 0;
}

/* `ctx` is a const char** and takes which kind of TADR build it is, for the log. */
static int to_ask_tadr(const unsigned char* b, DWORD size, void* ctx)
{
    if (!to_find(b, size, TO_TADR)) return 0;
    *(const char**)ctx = to_find(b, size, TO_REC) ? "TADR's recorder" : "a TADR build";
    return 1;
}

/* The file name of a path, as ACP for the log and the report; "?" when it will not convert. */
static void to_basename(const wchar_t* path, char* out, size_t cap)
{
    const wchar_t* base = wcsrchr(path, L'\\');
    if (!WideCharToMultiByte(CP_ACP, 0, base ? base + 1 : path, -1, out, (int)cap, NULL, NULL))
        strcpy(out, "?");
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
    to_basename(who, whoA, MAX_PATH);
    if (!*whoA) strcpy(whoA, "?");
    to_basename(full, fullA, MAX_PATH);
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

/* ---- 2. a TADR module already mapped: the inert entry point (tagpu_takeover.h) ---------- */

/* The next mapped image of the game folder that is neither Impure's nor the exe's, from `m`
   -- NULL at the end; `path` takes its full path. util_enumerate_modules walks the address
   space rather than the loader's list, so a module the loader has mapped and not yet
   initialised is found too, which is the whole point of pass 2. */
static HMODULE to_next_local(HMODULE m, const wchar_t* game, wchar_t* path)
{
    HMODULE exe = GetModuleHandleW(NULL);
    wchar_t dir[MAX_PATH];
    while ((m = util_enumerate_modules(m))) {
        if (m == (HMODULE)g_ddraw_module || m == exe) continue;
        if (!GetModuleFileNameW(m, path, MAX_PATH)) continue;
        if (!to_dir(path, dir, MAX_PATH) || _wcsicmp(dir, game)) continue;
        return m;
    }
    return NULL;
}

/* mov eax,1; ret 0Ch -- what a DllMain that succeeds and does nothing compiles to. */
static const unsigned char TO_INERT[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC2, 0x0C, 0x00 };

/* The NT headers of a mapped image, or NULL: everything below reads the header out of the
   module rather than the file, so a module whose file has been replaced under it is still
   measured as it is in memory. */
static const IMAGE_NT_HEADERS32* to_nt(HMODULE m)
{
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m;
    const IMAGE_NT_HEADERS32* nt;
    if (!m || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        (DWORD)dos->e_lfanew > 0x1000)
        return NULL;
    nt = (const IMAGE_NT_HEADERS32*)((const unsigned char*)m + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE &&
           nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC ? nt : NULL;
}

static int to_inert(HMODULE m)
{
    const IMAGE_NT_HEADERS32* nt = to_nt(m);
    unsigned char* p;
    DWORD ep, old;
    if (!nt) return 0;
    ep = nt->OptionalHeader.AddressOfEntryPoint;
    if (!ep || ep + sizeof TO_INERT > nt->OptionalHeader.SizeOfImage) return 0;
    p = (unsigned char*)m + ep;
    if (!VirtualProtect(p, sizeof TO_INERT, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(p, TO_INERT, sizeof TO_INERT);
    VirtualProtect(p, sizeof TO_INERT, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof TO_INERT);
    return 1;
}

void tagpu_takeover_tadr_init(void)
{
    wchar_t path[MAX_PATH], game[MAX_PATH];
    HMODULE m = NULL;
    if (!g_ddraw_module) return;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !to_dir(path, game, MAX_PATH)) return;
    if (to_off(game)) return;
    while ((m = to_next_local(m, game, path))) {
        const char* what = NULL;
        char base[MAX_PATH];
        if (!to_file_ask(path, to_ask_tadr, &what)) continue;
        to_basename(path, base, sizeof base);
        if (to_inert(m))
            tagpu_logf("takeover: %s is %s -- its entry point was made inert before the loader "
                       "called it, so none of its own code runs", base, what);
        else
            tagpu_logf("takeover: %s is %s and its entry point could not be made inert "
                       "(error %lu) -- its code runs", base, what, GetLastError());
    }
}

/* ---- 3. the exe's DirectPlay imports (tagpu_takeover.h) --------------------------------- */

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
   under too, or the exe would not bind to it. All stdcall.

   E_FAIL when Windows' dplayx.dll cannot be loaded, which the exe survives at each of its
   four call sites (DISASSEMBLED): the enum at 0x4CA435 discards the result and returns 1
   anyway; the lobby at 0x4CA4D7 tests it (jl 0x4CA4E0), and zeroes its out-pointer and three
   fields from the xor at 0x4CA4A0 before calling, so a failure leaves NULL and not a stale
   interface; the creates at 0x4CA667 and 0x4CA922 test it too (cmp/je 0x4CA66C, jl 0x4CA929)
   and return without touching what they would have got. Multiplayer is then unavailable;
   single player reaches the menu, and the exe's first DirectPlay call is the lobby one at
   start-up. */
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
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &m) ||
        m == (HMODULE)g_ddraw_module || !GetModuleFileNameW(m, path, MAX_PATH) ||
        !to_dir(path, dir, MAX_PATH) || _wcsicmp(dir, game) ||
        !GetProcAddress(m, "DirectPlayCreate"))
        return 0;
    to_basename(path, who, cap);
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
    nt = to_nt((HMODULE)exe);
    if (!nt) return;
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

/* ---- 4. the whole image against the exe file (tagpu_takeover.h) ------------------------- */

#define TO_MAX_MOD  32      /* modules of the game folder a target is tested against */
#define TO_MAX_FIND 32      /* findings kept whole; the count is exact whatever it is */
#define TO_BOX_FIND 8       /* of those, the ones the box has room for */
#define TO_SITE_B   8       /* bytes of a site shown, ours and the file's */

typedef struct { DWORD lo, hi; int tadr; char name[64]; } TO_MOD;
typedef struct {
    DWORD va, target;
    int mod, n;
    unsigned char mem[TO_SITE_B], file[TO_SITE_B];
} TO_FIND;

typedef struct {
    const unsigned char* exe;       /* the image in memory */
    DWORD base, image, stamp, bytes;
    TO_MOD mod[TO_MAX_MOD];
    int nmod;
    TO_FIND find[TO_MAX_FIND];
    int nfind, sections;
    int tadr;                       /* leading into a TADR module: what refuses the launch */
    int other;                      /* leading into another DLL of the folder: the mod's own */
    DWORD runs;
    const char* why;                /* NULL when the code really was compared */
} TO_SCAN;

static int to_mod_at(const TO_SCAN* sc, DWORD target)
{
    int i;
    for (i = 0; i < sc->nmod; i++)
        if (target >= sc->mod[i].lo && target < sc->mod[i].hi) return i;
    return -1;
}

/* The address a control transfer at p[s] leads to, or 0. `va` is the address of p[0]. Only
   the shapes a hook writes -- nothing here disassembles, and nothing here is a list of
   TADR's sites: the absolute-address test in to_scan_section catches a target this misses,
   and pass 4's job is to refuse, not to name the mechanism. */
static DWORD to_target(const TO_SCAN* sc, const unsigned char* p, DWORD n, DWORD va, DWORD s)
{
    DWORD imm;
    if (s + 5 <= n && (p[s] == 0xE8 || p[s] == 0xE9)) {          /* call/jmp rel32 */
        memcpy(&imm, p + s + 1, 4);
        return va + s + 5 + imm;                                 /* wraps as the CPU's does */
    }
    if (s + 6 <= n && p[s] == 0xFF && (p[s + 1] == 0x15 || p[s + 1] == 0x25)) {
        memcpy(&imm, p + s + 2, 4);                              /* the pointer's address */
        if (imm >= sc->base && imm - sc->base <= sc->image - 4) {
            DWORD t;
            memcpy(&t, sc->exe + (imm - sc->base), 4);
            return t;
        }
        return 0;
    }
    if (s + 6 <= n && p[s] == 0x68 && p[s + 5] == 0xC3) {         /* push imm32; ret */
        memcpy(&imm, p + s + 1, 4);
        return imm;
    }
    if (s + 7 <= n && p[s] == 0xB8 && p[s + 5] == 0xFF && p[s + 6] == 0xE0) {
        memcpy(&imm, p + s + 1, 4);                               /* mov eax,imm32; jmp eax */
        return imm;
    }
    return 0;
}

/* A CHANGED BYTE THAT LEADS INTO A MODULE OF THE GAME FOLDER. Only a TADR module refuses the
   launch: the Community Patch Loader rewrites three of the exe's import thunks into direct
   calls to the mod's own win32.dll (MEASURED 2026-09-27, Total Mayhem 11.3.0 and ProTA 4.8 at
   0x004E4708, 0x004E71A0 and 0x004EADF2, `FF 15` becoming `E8 rel32; nop`), and that is a byte
   the mod itself sets -- it stays. Both kinds are kept and logged; the count of each is what
   decides. */
static void to_found(TO_SCAN* sc, DWORD va, const unsigned char* mem, const unsigned char* file,
                     DWORD avail, DWORD target, int mod)
{
    TO_FIND* f;
    if (sc->mod[mod].tadr) sc->tadr++; else sc->other++;
    if (sc->nfind >= TO_MAX_FIND) return;
    f = &sc->find[sc->nfind++];
    f->va = va;
    f->target = target;
    f->mod = mod;
    f->n = (int)(avail < TO_SITE_B ? avail : TO_SITE_B);
    memcpy(f->mem, mem, (size_t)f->n);
    memcpy(f->file, file, (size_t)f->n);
}

static void to_scan_section(TO_SCAN* sc, const unsigned char* mem, const unsigned char* file,
                            DWORD n, DWORD va)
{
    DWORD i = 0;
    while (i < n) {
        DWORD a, b, s;
        if (mem[i] == file[i]) { i++; continue; }
        a = i;
        while (i < n && mem[i] != file[i]) i++;
        b = i;
        sc->runs++;
        /* The opcode of a changed jump can begin up to four bytes BEFORE the first byte that
           differs: a hook that reuses a stock E8 changes only its displacement. One finding a
           run is enough to refuse, and keeps the report one line a site. */
        for (s = a >= 4 ? a - 4 : 0; s < b; s++) {
            DWORD t = to_target(sc, mem, n, va, s);
            int k = t ? to_mod_at(sc, t) : -1;
            if (k < 0 && s >= a && s + 4 <= b) {
                memcpy(&t, mem + s, 4);          /* an absolute address the run itself holds */
                k = to_mod_at(sc, t);
            }
            if (k >= 0) {
                to_found(sc, va + s, mem + s, file + s, n - s, t, k);
                break;
            }
        }
    }
}

static void to_mods(TO_SCAN* sc, const wchar_t* game)
{
    wchar_t path[MAX_PATH];
    HMODULE m = NULL;
    while (sc->nmod < TO_MAX_MOD && (m = to_next_local(m, game, path))) {
        const IMAGE_NT_HEADERS32* nt = to_nt(m);
        const char* what = NULL;
        TO_MOD* d;
        if (!nt || !nt->OptionalHeader.SizeOfImage) continue;
        d = &sc->mod[sc->nmod++];
        d->lo = (DWORD)(size_t)m;
        d->hi = d->lo + nt->OptionalHeader.SizeOfImage;
        d->tadr = to_file_ask(path, to_ask_tadr, &what);
        to_basename(path, d->name, sizeof d->name);
    }
}

/* The exe file, mapped: its executable sections against the same bytes in memory. */
static int to_ask_image(const unsigned char* b, DWORD size, void* ctx)
{
    TO_SCAN* sc = (TO_SCAN*)ctx;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)to_at(b, size, 0, sizeof *dos);
    const IMAGE_NT_HEADERS32* nt;
    const IMAGE_SECTION_HEADER* sec;
    DWORD first, i;
    WORD n;
    sc->bytes = size;
    sc->why = "the exe file is not a 32-bit PE";
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return 1;
    nt = (const IMAGE_NT_HEADERS32*)to_at(b, size, (DWORD)dos->e_lfanew, sizeof *nt);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return 1;
    sc->stamp = nt->FileHeader.TimeDateStamp;
    /* Relocated, or a different file than the one loaded: every byte would differ and every
       comparison would be noise. Said in the log, never turned into a refusal. */
    sc->why = "the exe is loaded away from the base its file asks for";
    if (nt->OptionalHeader.ImageBase != sc->base) return 1;
    sc->why = "the exe file's section table is not where its header says";
    n = nt->FileHeader.NumberOfSections;
    first = (DWORD)((const unsigned char*)IMAGE_FIRST_SECTION(nt) - b);
    if (!to_at(b, size, first, (DWORD)n * sizeof *sec)) return 1;
    sec = (const IMAGE_SECTION_HEADER*)(b + first);
    sc->why = "the exe file has no executable section this build can read";
    for (i = 0; i < n; i++) {
        DWORD len = sec[i].Misc.VirtualSize < sec[i].SizeOfRawData ? sec[i].Misc.VirtualSize
                                                                  : sec[i].SizeOfRawData;
        if (!(sec[i].Characteristics & (IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE))) continue;
        if (!len || sec[i].VirtualAddress > sc->image || sc->image - sec[i].VirtualAddress < len)
            continue;
        if (!to_at(b, size, sec[i].PointerToRawData, len)) continue;
        sc->sections++;
        sc->why = NULL;
        to_scan_section(sc, sc->exe + sec[i].VirtualAddress, b + sec[i].PointerToRawData,
                        len, sc->base + sec[i].VirtualAddress);
    }
    return 1;
}

static void to_hex(char* out, const unsigned char* b, int n)
{
    int i;
    out[0] = 0;
    for (i = 0; i < n; i++) sprintf(out + strlen(out), i ? " %02X" : "%02X", b[i]);
}

/* One `text` from the scan's findings, in the shape tagpu_refuse.h asks for. */
static void to_report(TO_SCAN* sc, char* text, size_t cap)
{
    char line[512], mem[3 * TO_SITE_B], file[3 * TO_SITE_B], who[256] = "";
    int i, shown = 0;
    /* the TADR modules the findings actually lead into, not every one in the folder */
    for (i = 0; i < sc->nmod; i++) {
        int hit = 0, k;
        for (k = 0; k < sc->nfind; k++) hit |= sc->find[k].mod == i;
        if (hit && sc->mod[i].tadr && strlen(who) + strlen(sc->mod[i].name) + 3 < sizeof who)
            sprintf(who + strlen(who), "%s%s", *who ? ", " : "", sc->mod[i].name);
    }
    _snprintf(text, cap,
        "Total Annihilation's code has been changed in memory by TA Demo Recorder (TADR), so "
        "the game will now close before a battle can go wrong.\r\n"
        "\r\n"
        "WHY\r\n"
        "Every time it starts, Impure compares the game's code with TotalA.exe as it is on "
        "disk. %d place%s now lead into %s. Impure replaces what TADR does to the engine, and "
        "two programs patching the same engine is what makes a game crash later, or play by "
        "different rules from other players'.\r\n"
        "\r\n"
        "WHAT TO DO\r\n"
        "- Take TADR out of the game folder: %s. A mod's own files and its own TotalA.exe can "
        "stay -- Impure compares against the exe it finds, so a mod's own changes are not the "
        "problem.\r\n"
        "- If the folder has a file named tagpu_takeover.off, delete it: it lets TADR start.\r\n"
        "- Or report it: press Ctrl+C to copy this message and paste it into a new issue at\r\n"
        "github.com/Code-Herder/ta-impure-patch/issues\r\n"
        "The same report is saved in log\\startup-failure.txt\r\n"
        "\r\n"
        "--- report ---\r\n"
        "impure %s (%s)\r\n"
        "exe %lu bytes, PE stamp 0x%08lX, image 0x%08lX+0x%08lX\r\n"
        "%lu changed run%s in %d executable section%s, %d into TADR, %d into another DLL of "
        "the folder\r\n",
        sc->tadr, sc->tadr == 1 ? "" : "s", *who ? who : "a TADR DLL of the game folder",
        *who ? who : "tdraw.dll and tplayx.dll, or copies of them under other names",
        GIT_COMMIT, GIT_BRANCH, (unsigned long)sc->bytes, (unsigned long)sc->stamp,
        (unsigned long)sc->base, (unsigned long)sc->image,
        (unsigned long)sc->runs, sc->runs == 1 ? "" : "s", sc->sections,
        sc->sections == 1 ? "" : "s", sc->tadr, sc->other);
    text[cap - 1] = 0;
    for (i = 0; i < sc->nmod; i++) {
        _snprintf(line, sizeof line, "module 0x%08lX-0x%08lX %s%s\r\n",
                  (unsigned long)sc->mod[i].lo, (unsigned long)sc->mod[i].hi,
                  sc->mod[i].name, sc->mod[i].tadr ? " (TADR)" : "");
        line[sizeof line - 1] = 0;
        strncat(text, line, cap - strlen(text) - 1);
    }
    for (i = 0; i < sc->nfind; i++) {
        const TO_FIND* f = &sc->find[i];
        if (!sc->mod[f->mod].tadr) continue;      /* the box shows what refuses, the log all */
        if (++shown > TO_BOX_FIND) {
            _snprintf(line, sizeof line, "... and %d more, all of them in log\\tagpu.log\r\n",
                      sc->tadr - TO_BOX_FIND);
            line[sizeof line - 1] = 0;
            strncat(text, line, cap - strlen(text) - 1);
            break;
        }
        to_hex(mem, f->mem, f->n);
        to_hex(file, f->file, f->n);
        _snprintf(line, sizeof line, "0x%08lX -> 0x%08lX %s\r\n  now  %s\r\n  file %s\r\n",
                  (unsigned long)f->va, (unsigned long)f->target, sc->mod[f->mod].name,
                  mem, file);
        line[sizeof line - 1] = 0;
        strncat(text, line, cap - strlen(text) - 1);
    }
}

void tagpu_takeover_verify_image(void)
{
    static LONG once;
    static TO_SCAN sc;          /* 32 modules and 32 findings: not the stack */
    static char text[4096];
    wchar_t path[MAX_PATH], game[MAX_PATH];
    const IMAGE_NT_HEADERS32* nt;
    HMODULE exe;
    int i;

    if (InterlockedExchange(&once, 1)) return;
    if (!g_ddraw_module) return;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !to_dir(path, game, MAX_PATH)) return;
    if (to_off(game)) {
        tagpu_log("takeover: the exe's code was not compared with its file -- tagpu_takeover.off");
        return;
    }
    exe = GetModuleHandleW(NULL);
    nt = to_nt(exe);
    if (!nt) return;
    sc.exe = (const unsigned char*)exe;
    sc.base = (DWORD)(size_t)exe;
    sc.image = nt->OptionalHeader.SizeOfImage;
    to_mods(&sc, game);
    sc.why = "the exe file could not be read";
    to_file_ask(path, to_ask_image, &sc);
    if (sc.why) {
        tagpu_logf("takeover: the exe's code was not compared with its file -- %s", sc.why);
        return;
    }
    /* Every finding is logged, TADR's and the mod's own alike: a site leading into a DLL of
       the folder that carries no TADR marker is the mod's byte and stays, and the log is
       where it stays visible. */
    for (i = 0; i < sc.nfind; i++) {
        const TO_FIND* f = &sc.find[i];
        char mem[3 * TO_SITE_B], file[3 * TO_SITE_B];
        to_hex(mem, f->mem, f->n);
        to_hex(file, f->file, f->n);
        tagpu_logf("takeover:   0x%08lX -> 0x%08lX %s%s now %s file %s",
                   (unsigned long)f->va, (unsigned long)f->target, sc.mod[f->mod].name,
                   sc.mod[f->mod].tadr ? " (TADR)" : " (not TADR: the mod's own)", mem, file);
    }
    tagpu_logf("takeover: the exe's code against its file -- %lu changed run%s in %d executable "
               "section%s, %d leading into a TADR module of the game folder and %d into another "
               "of its DLLs, of %d looked at", (unsigned long)sc.runs, sc.runs == 1 ? "" : "s",
               sc.sections, sc.sections == 1 ? "" : "s", sc.tadr, sc.other, sc.nmod);
    if (!sc.tadr) return;
    tagpu_logf("takeover: FAILED -- %d place%s of the game's code lead into a TADR module",
               sc.tadr, sc.tadr == 1 ? "" : "s");
    to_report(&sc, text, sizeof text);
    tagpu_refuse(text);
}
