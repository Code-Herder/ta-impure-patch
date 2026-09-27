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
    /* PAGE_READWRITE, not PAGE_EXECUTE_READWRITE: no thread runs this module's code while the
       eight bytes are being written -- the loader has not called its entry point yet -- so the
       page never needs to be writable and executable at once, and a foreign module's image
       briefly made RWX is the shape behaviour-based anti-virus watches for. The protection it
       had is put back, and the instruction cache flushed, before anything can run. */
    if (!VirtualProtect(p, sizeof TO_INERT, PAGE_READWRITE, &old)) return 0;
    memcpy(p, TO_INERT, sizeof TO_INERT);
    VirtualProtect(p, sizeof TO_INERT, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, sizeof TO_INERT);
    return 1;
}

/* THE PRECONDITION OF MAKING ANYTHING INERT: no module of the game folder has been initialised
   yet, so none of the entry points below is one the loader has already called or is calling.
   The loader initialises the exe's imports as a post-order walk in import-directory order, and
   Impure imports nothing from the game folder, so the condition is exactly "the first descriptor
   of the exe that leads into the game folder is Impure's": every other game-folder module is in a
   later descriptor's subtree and runs after us.

   It is not an assumption about the retail exe. The 3.9.02 and Escalation exes import `TDRAW` /
   `TAESC` and no `DDRAW` at all (DISASSEMBLED: objdump -p), so on those routes TADR's `DllMain`
   is what loads Impure and is running while this would write -- and there this returns 0 and the
   pass is skipped. Part 4's comparison still refuses such a launch.

   EVERY SLOT of a descriptor is looked at, not its first: a descriptor can name functions from
   more than one module only in the sense that its slots are all one module's, but a slot the
   loader could not bind is left as the file's value, and reading only slot 0 would then miss the
   descriptor entirely. WHAT IT STILL CANNOT SEE: a game-folder module that is not in the exe's
   import table at all -- pulled in as the dependency of an earlier descriptor's module, or by a
   forwarded export. Such a module is initialised before us and is made inert anyway; the log's
   "none of its own code runs" would be wrong about it, and part 4 is what answers for it. No
   fixture has one (MEASURED 2026-09-27: objdump -p over every setup's exe and DLLs). */
static int to_first_local_is_ours(const BYTE* exe, DWORD image, const wchar_t* game)
{
    DWORD at, end;
    const IMAGE_NT_HEADERS32* nt = to_nt((HMODULE)exe);
    const IMAGE_DATA_DIRECTORY* dd;
    if (!nt || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return 0;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || dd->VirtualAddress >= image) return 0;
    end = image - (DWORD)sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (at = dd->VirtualAddress; at <= end; at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const IMAGE_IMPORT_DESCRIPTOR* d = (const IMAGE_IMPORT_DESCRIPTOR*)(exe + at);
        HMODULE m = NULL;
        wchar_t path[MAX_PATH], dir[MAX_PATH];
        DWORD k;
        int local = 0;
        if (!d->FirstThunk) break;
        for (k = 0; d->FirstThunk + (k + 1) * sizeof(DWORD) <= image && !local; k++) {
            DWORD target = ((const DWORD*)(exe + d->FirstThunk))[k];
            if (!target) break;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCWSTR)(size_t)target, &m) || !m)
                continue;
            if (GetModuleFileNameW(m, path, MAX_PATH) && to_dir(path, dir, MAX_PATH) &&
                !_wcsicmp(dir, game))
                local = 1;
        }
        if (!local) continue;
        return m == (HMODULE)g_ddraw_module;
    }
    return 1;           /* the exe imports nothing from the game folder: nothing has run */
}

/* Whether the module's PE TLS directory names a callback the loader will still call at
   DLL_PROCESS_ATTACH. Making the entry point inert does not cover one, and this write does not
   either: it is logged, so a build that ever carries one is visible rather than silently outside
   the invariant. MEASURED 2026-09-27: both tdraw.dll builds of the fixtures carry a TLS directory
   (Mayhem RVA 0x6E240, ProTA 0x81F00) whose callback array begins with NULL, so nothing runs. */
static int to_tls_callbacks(HMODULE m)
{
    const IMAGE_NT_HEADERS32* nt = to_nt(m);
    const IMAGE_DATA_DIRECTORY* dd;
    const IMAGE_TLS_DIRECTORY32* tls;
    DWORD image, cb;
    if (!nt || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS) return 0;
    image = nt->OptionalHeader.SizeOfImage;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dd->VirtualAddress || dd->VirtualAddress + sizeof *tls > image) return 0;
    tls = (const IMAGE_TLS_DIRECTORY32*)((const unsigned char*)m + dd->VirtualAddress);
    cb = (DWORD)tls->AddressOfCallBacks;
    if (cb < (DWORD)(size_t)m || cb - (DWORD)(size_t)m > image - sizeof(DWORD)) return 0;
    return *(const DWORD*)(size_t)cb != 0;
}

void tagpu_takeover_tadr_init(void)
{
    wchar_t path[MAX_PATH], game[MAX_PATH];
    BYTE* exe = (BYTE*)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS32* nt = to_nt((HMODULE)exe);
    HMODULE m = NULL;
    if (!g_ddraw_module || !nt) return;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !to_dir(path, game, MAX_PATH)) return;
    if (to_off(game)) return;
    if (!to_first_local_is_ours(exe, nt->OptionalHeader.SizeOfImage, game)) {
        tagpu_log("takeover: a DLL of the game folder started before Impure, so nothing of TADR's "
                  "is made inert -- its code is already running, and the comparison at the first "
                  "DirectDraw call is what answers for this launch");
        return;
    }
    while ((m = to_next_local(m, game, path))) {
        const char* what = NULL;
        char base[MAX_PATH];
        if (!to_file_ask(path, to_ask_tadr, &what)) continue;
        to_basename(path, base, sizeof base);
        if (to_inert(m))
            tagpu_logf("takeover: %s is %s -- its entry point was made inert before the loader "
                       "called it, so none of its own code runs%s", base, what,
                       to_tls_callbacks(m) ? ", except the TLS callbacks it carries, which the "
                                             "loader calls and this does not touch" : "");
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
   is not Impure: its file name (for the log) in `who`, else 0.

   THE EXPORT TEST READS THE FILE, never the loaded module: GetProcAddress on a FORWARDED export
   makes the loader load and initialise the target, and every Patch Loader's dplayx.dll forwards
   all nine of its exports to tplayx (DISASSEMBLED: objdump -p, the loader's, Total Mayhem's and
   ProTA's copies). Asking it here -- from DllMain, under the loader lock -- would start the
   recorder out of the loader's own order, on the very paths where pass 2 did not make it inert.
   The file's export table answers the same question and runs none of its code; every dplayx of
   the fixtures exports the NAME DirectPlayCreate (MEASURED 2026-09-27, the loader's, the 2006
   recorder's, Mayhem's and ProTA's), so nothing is missed by not resolving it. */
static int dp_foreign(const void* target, const wchar_t* game, char* who, size_t cap)
{
    HMODULE m = NULL;
    wchar_t path[MAX_PATH], dir[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &m) ||
        m == (HMODULE)g_ddraw_module || !GetModuleFileNameW(m, path, MAX_PATH) ||
        !to_dir(path, dir, MAX_PATH) || _wcsicmp(dir, game) ||
        !to_file_exports(path, "DirectPlayCreate"))
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
        /* The slots are about to hold addresses of code in Impure's module for the life of the
           process. PIN it first: a FreeLibrary that unmapped Impure would leave the exe calling
           into nothing, and the reference LoadLibrary gave whoever loaded us is not ours to
           rely on. Pinning cannot be undone, which is exactly what is wanted here. */
        {
            HMODULE pinned = NULL;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    (LPCWSTR)(void*)dp_create, &pinned)) {
                tagpu_logf("takeover: %s's slots lead into %s, but Impure's own module could not "
                           "be pinned (error %lu) -- the descriptor is left alone", dll, who,
                           GetLastError());
                continue;
            }
        }
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
    int mod, n, slot;               /* slot: an import slot of the exe, not a site of its code */
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
    int maybe;                      /* a run whose bytes merely HOLD such an address: see below */
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
   the shapes a hook writes -- nothing here disassembles, and nothing here is a list of TADR's
   sites. THIS IS WHAT A REFUSAL RESTS ON: an instruction that transfers control names its target,
   where four bytes that merely hold a value do not (to_scan_section). */
static DWORD to_target(const TO_SCAN* sc, const unsigned char* p, DWORD n, DWORD va, DWORD s)
{
    DWORD imm;
    if (s + 5 <= n && (p[s] == 0xE8 || p[s] == 0xE9)) {          /* call/jmp rel32 */
        memcpy(&imm, p + s + 1, 4);
        return va + s + 5 + imm;                                 /* wraps as the CPU's does */
    }
    if (s + 6 <= n && p[s] == 0xFF && (p[s + 1] == 0x15 || p[s + 1] == 0x25)) {
        memcpy(&imm, p + s + 2, 4);                              /* the pointer's address */
        /* The pointer is read only where the read is safe by construction: inside the exe's own
           image, or inside a module of the game folder whose extent the table above holds. Both
           are mapped for the life of the process and bounded by their SizeOfImage, so no probe
           and no exception handler is needed -- and a hook of the form `jmp [ptr in TADR's own
           module]` is decoded rather than left to the coincidence count. Anywhere else the
           pointer is not read at all: the suite, which reads the process from outside, decodes
           that case (tacompat.py decode_run) and this does not. */
        if (imm >= sc->base && imm - sc->base <= sc->image - 4)
            return *(const DWORD*)(sc->exe + (imm - sc->base));
        if (to_mod_at(sc, imm) >= 0 && to_mod_at(sc, imm + 3) >= 0)
            return *(const DWORD*)(size_t)imm;
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
    f->slot = 0;
    f->n = (int)(avail < TO_SITE_B ? avail : TO_SITE_B);
    memcpy(f->mem, mem, (size_t)f->n);
    memcpy(f->file, file, (size_t)f->n);
}

/* A CHANGED RUN WHOSE BYTES MERELY HOLD an address inside one of the modules -- at any offset,
   aligned to nothing. That is a coincidence, not a hook, and it is COUNTED AND NOT REFUSED:
   the four bytes are as likely to be the middle of an instruction or the displacement of a jump
   as a pointer, and which of them happen to look like an address depends on where the loader put
   a DLL that day. MEASURED on Windows 2026-09-27, both halves of it: `8B 96 92 00` -- the middle
   of a `mov esi,[esi+0x92]` of ours -- reads as 0x0092968B, and Total Mayhem's recorder was
   mapped at 0x00910000, so three of Impure's own sites refused that install; and a rel32
   displacement of ours to a stub above the image reads as 0x020F0000-something, where Windows
   maps the recorder beside the retail exe, which refused `retail+tadr1` over 32 more. Every TADR
   hook actually measured -- 24 on the 2006 recorder, 6 on the entry-point route, 13 to 18 of a
   tdraw's -- is found by its instruction, so nothing is lost by not refusing on a value. */
static int to_holds_address(TO_SCAN* sc, const unsigned char* mem, DWORD a, DWORD b)
{
    DWORD s, t;
    for (s = a; s + 4 <= b; s++) {
        memcpy(&t, mem + s, 4);
        if (to_mod_at(sc, t) >= 0) return 1;
    }
    return 0;
}

static void to_scan_section(TO_SCAN* sc, const unsigned char* mem, const unsigned char* file,
                            DWORD n, DWORD va)
{
    DWORD i = 0;
    while (i < n) {
        DWORD a, b, s;
        int found = 0, want;
        if (mem[i] == file[i]) { i++; continue; }
        a = i;
        while (i < n && mem[i] != file[i]) i++;
        b = i;
        sc->runs++;
        /* The opcode of a changed jump can begin up to FIVE bytes before the first byte that
           differs: a hook that reuses a stock instruction changes only the operand, and
           `FF 15`/`FF 25` carry theirs at offsets 2 to 5, so a repointed slot address whose last
           byte alone differs begins five bytes back. E8, E9, push/ret and mov/jmp all end at
           offset 4 and were covered by four.

           ONE FINDING A RUN, BUT THE RIGHT ONE. A run can decode into more than one module --
           the bytes before it may be a stock `FF 15` through an import slot that leads into the
           mod's own WIN32.dll -- and only a TADR target refuses, so the run is scanned for a TADR
           target first (`want`) and for any other module's only if it has none. Stopping at the
           first hit of either kind let a non-TADR one hide a TADR hook in the same run. */
        for (want = 1; want >= 0 && !found; want--)
            for (s = a >= 5 ? a - 5 : 0; s < b; s++) {
                DWORD t = to_target(sc, mem, n, va, s);
                int k = t ? to_mod_at(sc, t) : -1;
                if (k < 0 || sc->mod[k].tadr != want) continue;
                to_found(sc, va + s, mem + s, file + s, n - s, t, k);
                found = 1;
                break;
            }
        if (!found && to_holds_address(sc, mem, a, b)) sc->maybe++;
    }
}

static void to_mods(TO_SCAN* sc, const wchar_t* game)
{
    wchar_t path[MAX_PATH];
    HMODULE m = NULL;
    while ((m = to_next_local(m, game, path))) {
        const IMAGE_NT_HEADERS32* nt = to_nt(m);
        const char* what = NULL;
        TO_MOD* d;
        if (!nt || !nt->OptionalHeader.SizeOfImage) continue;
        if (sc->nmod >= TO_MAX_MOD) {
            /* A module left out of the table is one no target can be found in, so the count in
               the log is not "0 findings" but "0 of what was looked at": say which it is. */
            tagpu_log("takeover: more than 32 DLLs of the game folder are loaded -- the ones past "
                      "the 32nd are not in the comparison, and a target inside one is not seen");
            return;
        }
        d = &sc->mod[sc->nmod++];
        d->lo = (DWORD)(size_t)m;
        d->hi = d->lo + nt->OptionalHeader.SizeOfImage;
        d->tadr = to_file_ask(path, to_ask_tadr, &what);
        to_basename(path, d->name, sizeof d->name);
    }
}

/* EVERY IMPORT SLOT OF THE EXE, which is the other way a call can leave the image: the code
   that reaches a slot is stock, so the comparison above never looks at it, and the file holds
   no bound address to compare with. What makes a slot wrong is not a changed byte but where it
   leads -- into a TADR module of the game folder -- and by this point every DllMain has run, so
   whatever rewrote one has done it.

   This is also the failure mode pass 3 leaves behind: a descriptor it could not name every slot
   of keeps the recorder's addresses, and the recorder is now inert, so the exe's first
   DirectPlay call would enter uninitialised Delphi code. Refusing is the answer to that, not a
   crash. The exe's own TADR imports on the routes where TADR loads Impure (TDRAW, TPLAYX) are
   not reached: those launches are refused by the limits table first. */
static void to_imports(TO_SCAN* sc, const unsigned char* exe, DWORD image)
{
    const IMAGE_NT_HEADERS32* nt = to_nt((HMODULE)exe);
    const IMAGE_DATA_DIRECTORY* dd;
    DWORD at, end;
    if (!nt || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || dd->VirtualAddress >= image) return;
    end = image - (DWORD)sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (at = dd->VirtualAddress; at <= end; at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const IMAGE_IMPORT_DESCRIPTOR* d = (const IMAGE_IMPORT_DESCRIPTOR*)(exe + at);
        DWORD k, slots = d->FirstThunk;
        if (!slots) break;
        /* a bound IAT keeps the null terminator the file's array ends with */
        for (k = 0; slots + (k + 1) * sizeof(DWORD) <= image; k++) {
            DWORD target = ((const DWORD*)(exe + slots))[k];
            int mod;
            if (!target) break;
            mod = to_mod_at(sc, target);
            if (mod < 0 || !sc->mod[mod].tadr) continue;
            if (sc->nfind < TO_MAX_FIND) {
                TO_FIND* f = &sc->find[sc->nfind++];
                f->va = sc->base + slots + k * (DWORD)sizeof(DWORD);
                f->target = target;
                f->mod = mod;
                f->slot = 1;
                f->n = 0;
            }
            sc->tadr++;
        }
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
        "the folder, %d merely holding such an address\r\n",
        sc->tadr, sc->tadr == 1 ? "" : "s", *who ? who : "a TADR DLL of the game folder",
        *who ? who : "tdraw.dll and tplayx.dll, or copies of them under other names",
        GIT_COMMIT, GIT_BRANCH, (unsigned long)sc->bytes, (unsigned long)sc->stamp,
        (unsigned long)sc->base, (unsigned long)sc->image,
        (unsigned long)sc->runs, sc->runs == 1 ? "" : "s", sc->sections,
        sc->sections == 1 ? "" : "s", sc->tadr, sc->other, sc->maybe);
    text[cap - 1] = 0;
    for (i = 0; i < sc->nmod; i++) {
        _snprintf(line, sizeof line, "module 0x%08lX-0x%08lX %s%s\r\n",
                  (unsigned long)sc->mod[i].lo, (unsigned long)sc->mod[i].hi,
                  sc->mod[i].name, sc->mod[i].tadr ? " (TADR)" : "");
        line[sizeof line - 1] = 0;
        strncat(text, line, cap - strlen(text) - 1);
    }
    for (i = 0; i < sc->nfind && shown < TO_BOX_FIND; i++) {
        const TO_FIND* f = &sc->find[i];
        if (!sc->mod[f->mod].tadr) continue;      /* the box shows what refuses, the log all */
        shown++;
        if (f->slot) {
            _snprintf(line, sizeof line, "0x%08lX -> 0x%08lX %s (an import slot)\r\n",
                      (unsigned long)f->va, (unsigned long)f->target, sc->mod[f->mod].name);
            line[sizeof line - 1] = 0;
            strncat(text, line, cap - strlen(text) - 1);
            continue;
        }
        to_hex(mem, f->mem, f->n);
        to_hex(file, f->file, f->n);
        _snprintf(line, sizeof line, "0x%08lX -> 0x%08lX %s\r\n  now  %s\r\n  file %s\r\n",
                  (unsigned long)f->va, (unsigned long)f->target, sc->mod[f->mod].name,
                  mem, file);
        line[sizeof line - 1] = 0;
        strncat(text, line, cap - strlen(text) - 1);
    }
    if (shown < sc->tadr) {
        _snprintf(line, sizeof line, "... and %d more, all of them in log\\tagpu.log\r\n",
                  sc->tadr - shown);
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
    to_imports(&sc, sc.exe, sc.image);
    sc.why = "the exe file could not be read";
    to_file_ask(path, to_ask_image, &sc);
    if (sc.why)
        /* The IMPORT SLOTS were still read, and they do not depend on the file: what makes a slot
           wrong is where it leads. So the code comparison being impossible -- an exe someone else
           holds open for writing, or one loaded away from its own ImageBase -- must not throw away
           a slot that leads into a module whose code cannot run. That is pass 3's own failure mode
           (a descriptor it could not name every slot of), and the first DirectPlay call would
           enter uninitialised Delphi. The findings below are logged either way. */
        tagpu_logf("takeover: the exe's code was not compared with its file -- %s; its import "
                   "slots were read", sc.why);
    /* Every finding is logged, TADR's and the mod's own alike: a site leading into a DLL of
       the folder that carries no TADR marker is the mod's byte and stays, and the log is
       where it stays visible. */
    for (i = 0; i < sc.nfind; i++) {
        const TO_FIND* f = &sc.find[i];
        char mem[3 * TO_SITE_B], file[3 * TO_SITE_B];
        if (f->slot) {
            tagpu_logf("takeover:   0x%08lX -> 0x%08lX %s (TADR) -- an import slot of the exe",
                       (unsigned long)f->va, (unsigned long)f->target, sc.mod[f->mod].name);
            continue;
        }
        to_hex(mem, f->mem, f->n);
        to_hex(file, f->file, f->n);
        tagpu_logf("takeover:   0x%08lX -> 0x%08lX %s%s now %s file %s",
                   (unsigned long)f->va, (unsigned long)f->target, sc.mod[f->mod].name,
                   sc.mod[f->mod].tadr ? " (TADR)" : " (not TADR: the mod's own)", mem, file);
    }
    if (sc.why) {
        if (!sc.tadr) return;
        tagpu_logf("takeover: FAILED -- %d import slot%s of the exe lead into a TADR module",
                   sc.tadr, sc.tadr == 1 ? "" : "s");
        to_report(&sc, text, sizeof text);
        tagpu_refuse(text);
        return;
    }
    tagpu_logf("takeover: the exe's code against its file, and its import slots -- %lu changed "
               "run%s in %d executable section%s, %d leading into a TADR module of the game folder "
               "and %d into another of its DLLs, of %d looked at; %d run%s such an address without "
               "an instruction that goes there, which is a coincidence and is not counted",
               (unsigned long)sc.runs, sc.runs == 1 ? "" : "s", sc.sections,
               sc.sections == 1 ? "" : "s", sc.tadr, sc.other, sc.nmod,
               sc.maybe, sc.maybe == 1 ? " holds" : "s hold");
    if (!sc.tadr) return;
    tagpu_logf("takeover: FAILED -- %d place%s of the game's code lead into a TADR module",
               sc.tadr, sc.tadr == 1 ? "" : "s");
    to_report(&sc, text, sizeof text);
    tagpu_refuse(text);
}
