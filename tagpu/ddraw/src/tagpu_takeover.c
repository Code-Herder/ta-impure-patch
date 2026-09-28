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

/* The module the loader bound an import descriptor's name to: the one loaded under that name.
   NULL when the name does not end inside the image or nothing of that name is loaded.
   GetModuleHandleA loads nothing, so it is safe from DllMain: a lookup under a lock the loader
   takes for lookups (Wine's loader lock, held here already; Windows 10's module table lock). */
static HMODULE to_descriptor_module(const BYTE* exe, DWORD image, const IMAGE_IMPORT_DESCRIPTOR* d)
{
    char name[MAX_PATH];
    DWORD n;
    if (!d->Name || d->Name >= image) return NULL;
    for (n = 0; n < sizeof name - 1 && n < image - d->Name && exe[d->Name + n]; n++)
        name[n] = (char)exe[d->Name + n];
    if (n == sizeof name - 1 || n == image - d->Name) return NULL;     /* not terminated */
    name[n] = 0;
    return GetModuleHandleA(name);
}

/* WHICH IMPORT DESCRIPTOR OF THE EXE NAMES A MODULE FIRST: its index, -1 when none does.

   THE PRECONDITION OF MAKING A MODULE INERT is that the loader has not called its entry point
   yet. The loader initialises the exe's imports as a post-order walk in import-directory order,
   and the DllMain running right now is the one of the module Impure was loaded from -- Impure's
   own on the routes where the exe imports DDRAW, TADR's on the routes where it imports TDRAW or
   TAESC and 1d has just stopped it. So a module is untouched exactly when every descriptor that
   names it comes AFTER the descriptor that names the module we are inside: its subtree has not
   been walked. That is why the answer is an index and not a yes or no -- on the routes where TADR
   loads Impure its own descriptor is first, and the recorder's is later, so the recorder can
   still be made inert even though TADR itself cannot. to_loader_entered is the second,
   independent half of the same precondition.

   A DESCRIPTOR NAMES A MODULE in either of two ways, and both count. By its NAME, when the module
   loaded under that name is this one -- the loader's own binding, which no later write can
   change. By a SLOT whose value lies inside the module -- how a forwarder is seen: the Patch
   Loader's dplayx.dll forwards to tplayx.dll, so the DPLAYX descriptor's name resolves to the
   forwarding DLL and only its slots lead into the recorder. The name is not optional: a slot is
   data anyone in the process can rewrite before this runs. MEASURED 2026-09-27 on Windows 10
   19041, Total Mayhem 11.3.0, the first launch of an exe at a path Windows had not started it
   from: the exe's one DDRAW slot, DirectDrawCreate at 0x4FC02C, held apphelp.dll+0x68B10 -- the
   compatibility engine's hook -- when Impure's DllMain ran, and Impure's own on every later launch.
   Found by slot alone, Impure's descriptor was then "none", 1d was asked instead, and the recorder
   ran. `foreign` (may be NULL) gets the first slot of a descriptor found by name alone, so that
   case is logged; 0 otherwise.

   EVERY SLOT of a descriptor is looked at, not its first: a slot the loader could not bind is
   left as the file's value, and reading only slot 0 would then miss the descriptor entirely.
   WHAT IT CANNOT SEE: that a module is also a dependency of an EARLIER descriptor's module. Then
   the loader initialised it inside that earlier subtree although its own descriptor comes later,
   and this index says "not yet" -- to_loader_entered is what keeps such a module from being made
   inert. A module the exe does not import at all gets -1 and is left running. No fixture has
   either (MEASURED 2026-09-27: objdump -p over every setup's exe and DLLs). */
static int to_descriptor_of(const BYTE* exe, DWORD image, HMODULE m, DWORD* foreign)
{
    DWORD at, end;
    const IMAGE_NT_HEADERS32* nt = to_nt((HMODULE)exe);
    const IMAGE_DATA_DIRECTORY* dd;
    const IMAGE_NT_HEADERS32* mnt = to_nt(m);
    int k = 0;
    if (foreign) *foreign = 0;
    if (!nt || !mnt || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        return -1;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || dd->VirtualAddress >= image) return -1;
    end = image - (DWORD)sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (at = dd->VirtualAddress; at <= end; at += sizeof(IMAGE_IMPORT_DESCRIPTOR), k++) {
        const IMAGE_IMPORT_DESCRIPTOR* d = (const IMAGE_IMPORT_DESCRIPTOR*)(exe + at);
        DWORD j;
        if (!d->FirstThunk) break;
        if (d->FirstThunk >= image) continue;          /* before the addition, so it cannot wrap */
        for (j = 0; (j + 1) * sizeof(DWORD) <= image - d->FirstThunk; j++) {
            DWORD t = ((const DWORD*)(exe + d->FirstThunk))[j];
            if (!t) break;
            if (t - (DWORD)(size_t)m >= mnt->OptionalHeader.SizeOfImage) continue;
            return k;                                  /* the first descriptor that names it */
        }
        if (to_descriptor_module(exe, image, d) == m) {
            if (foreign && d->FirstThunk <= image - sizeof(DWORD))
                *foreign = ((const DWORD*)(exe + d->FirstThunk))[0];
            return k;
        }
    }
    return -1;
}

/* The TIB, one field at a time. One instruction, where mingw's `__readfsdword` expands to a
   subscript of a zero-length array that GCC reports as out of bounds for a non-zero offset. */
static __inline DWORD nest_fs(unsigned off)
{
    DWORD v;
    __asm__ __volatile__("movl %%fs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

/* WHETHER THE LOADER HAS BEGUN INITIALISING A MODULE, from its own record: the module is on the
   process's initialisation-order list, PEB_LDR_DATA.InInitializationOrderModuleList. The loader
   puts a module there before its TLS callbacks and its entry point are called, so a module NOT on
   it is one whose code the loader has not run. MEASURED 2026-09-27 from inside Impure's DllMain
   beside Total Mayhem 11.3.0, on Windows 10 19041 and on Wine 9.0: both list DDRAW.dll (the module
   whose entry point is running) and neither lists tplayx.dll (whose entry point has not been
   called). Wine's process_attach inserts it after the dependencies and before MODULE_InitDLL;
   Windows 8 and later insert in LdrpInitializeNode before the initialisers [INFERRED for every
   Windows build but the one measured]. Windows 7 and earlier are not measured and [INFERRED from
   ReactOS] list every static import during the import walk, before any initialiser: there every
   recorder reads as entered, is left running, and the image comparison refuses the launch --
   the safe direction.

   A LIFETIME, NOT A SNAPSHOT: this runs from DllMain, under the loader lock, and the loader
   changes the list only under that lock (Wine's loader_section; Windows' LdrpLoaderLock
   [INFERRED], the parallel loader's workers map and snap and never initialise) -- so the answer
   holds until DllMain returns, which is after the entry point has been written. The 32-bit
   layout, the same on both loaders: PEB+0x30 in the TIB, PEB_LDR_DATA at PEB+0x0C, the list head
   at +0x1C, and in LDR_DATA_TABLE_ENTRY the initialisation-order links at +0x10 and DllBase at
   +0x18.

   ONLY A POSITIVE ANSWER COUNTS AS "NOT ENTERED": the walk must reach the end of a list that holds
   Impure's own module, which is on it while this DllMain runs (MEASURED above) -- that is what
   makes it the live list rather than whatever the pointers happen to lead to. No PEB, no loader
   data, a NULL link, more entries than any process loads, or a list without Impure on it, all
   count as entered. A module that unlinked itself after running is the case the anchor exists
   for: absent from a list that is otherwise real, it would read as not entered -- so it must also
   pass the descriptor half, which it cannot unlink itself from. */
static int to_loader_entered(HMODULE m)
{
    const BYTE* peb = (const BYTE*)(size_t)nest_fs(0x30);
    const BYTE* ldr;
    const LIST_ENTRY *head, *e;
    unsigned n = 0;
    int self = 0;
    if (!peb) return 1;
    ldr = *(const BYTE* const*)(peb + 0x0C);
    if (!ldr) return 1;
    head = (const LIST_ENTRY*)(ldr + 0x1C);
    for (e = head->Flink; e != head; e = e->Flink) {
        HMODULE base;
        if (!e || ++n > 4096) return 1;
        base = *(HMODULE const*)((const BYTE*)e - 0x10 + 0x18);
        if (base == m) return 1;
        if (base == g_ddraw_module) self = 1;
    }
    return !self;
}

/* Whether the module's PE TLS directory names a callback the loader calls whatever the entry point
   holds. Making the entry point inert does not cover one, and this write does not either: it is
   logged, so a build carrying one is visible rather than silently outside the invariant.

   MEASURED 2026-09-27 over the fixtures' 18 TADR modules: no recorder carries a TLS directory at
   all (tplayx, eplayx, zplayx, the 2006 Dplayx.dll, nine of nine); Total Mayhem's and ProTA's
   tdraw.dll carry one whose callback array begins with NULL; gammata's carries none; the six
   remaining tdraw/TAESC builds carry two callbacks each, gated on DLL_THREAD_ATTACH and on
   DLL_THREAD_DETACH / DLL_PROCESS_DETACH, so neither does anything at DLL_PROCESS_ATTACH. Nothing
   runs today because the sets are disjoint: the modules made inert here are recorders, and a live
   callback array is only in a tdraw/TAESC -- which on the routes where the exe imports TADR is the
   module Impure is running inside, and the walk skips that one by its handle. A LIVE CALLBACK
   ARRAY IS A MODULE TO REFUSE, NOT ONE TO MAKE INERT: an inert entry
   point leaves the module's CRT start-up unrun, so its TLS index is never allocated, and the
   DLL_THREAD_ATTACH callback would index another module's TLS block on every thread the game
   creates (research/notes/compat/takeover.md, part 1). */
static int to_tls_callbacks(HMODULE m)
{
    const IMAGE_NT_HEADERS32* nt = to_nt(m);
    const IMAGE_DATA_DIRECTORY* dd;
    const IMAGE_TLS_DIRECTORY32* tls;
    DWORD image, cb;
    if (!nt || nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS) return 0;
    image = nt->OptionalHeader.SizeOfImage;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dd->VirtualAddress || dd->VirtualAddress >= image ||
        dd->VirtualAddress > image - sizeof *tls) return 0;   /* no addition that can wrap */
    tls = (const IMAGE_TLS_DIRECTORY32*)((const unsigned char*)m + dd->VirtualAddress);
    cb = (DWORD)tls->AddressOfCallBacks;
    if (cb < (DWORD)(size_t)m || cb - (DWORD)(size_t)m > image - sizeof(DWORD)) return 0;
    return *(const DWORD*)(size_t)cb != 0;
}

/* 1d, below: the pass for the routes where this one's invariant cannot hold. */
static HMODULE tagpu_takeover_nested_init(const wchar_t* game, int* kme);

void tagpu_takeover_tadr_init(void)
{
    wchar_t path[MAX_PATH], game[MAX_PATH];
    BYTE* exe = (BYTE*)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS32* nt = to_nt((HMODULE)exe);
    HMODULE m = NULL, inside;
    DWORD image, hooked;
    int kme;
    if (!g_ddraw_module || !nt) return;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !to_dir(path, game, MAX_PATH)) return;
    if (to_off(game)) return;
    image = nt->OptionalHeader.SizeOfImage;

    /* WHOSE DllMain THIS IS RUNNING INSIDE. Impure's own, unless the exe imports no DDRAW and
       TADR loaded us -- then 1d stops the rest of that DllMain and names the module, and the
       walk below is measured against ITS descriptor instead of Impure's. */
    inside = g_ddraw_module;
    kme = to_descriptor_of(exe, image, g_ddraw_module, &hooked);
    if (kme >= 0 && hooked) {
        HMODULE owner = NULL;
        wchar_t opath[MAX_PATH];
        char obase[MAX_PATH] = "no module";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)(size_t)hooked, &owner) &&
            GetModuleFileNameW(owner, opath, MAX_PATH))
            to_basename(opath, obase, sizeof obase);
        tagpu_logf("takeover: the exe's import of Impure leads into %s (0x%08lX), a hook put "
                   "there before Impure started -- its place in the load order is taken from "
                   "the name the loader bound, descriptor %d", obase, (unsigned long)hooked, kme);
    }
    if (kme < 0) {
        inside = tagpu_takeover_nested_init(game, &kme);
        if (!inside) return;        /* 1d said why; part 3 answers for this launch */
        /* 1d stopped a module no descriptor of the exe names: "later than the module we are
           inside" means nothing then, and every module would pass it. Both halves, or none. */
        if (kme < 0) {
            tagpu_log("takeover: the module 1d stopped is not in the exe's import table, so no "
                      "module's place in the load order can be compared with it -- nothing more "
                      "is made inert, and the comparison at the first DirectDraw call answers "
                      "for this launch");
            return;
        }
    }

    while ((m = to_next_local(m, game, path))) {
        const char* what = NULL;
        char base[MAX_PATH];
        int kfirst;
        if (m == inside) continue;      /* 1d stopped this one; its entry point is inert already */
        if (!to_file_ask(path, to_ask_tadr, &what)) continue;
        to_basename(path, base, sizeof base);
        kfirst = to_descriptor_of(exe, image, m, NULL);
        /* Its subtree must not have been walked yet: every descriptor naming it comes after the
           one we are inside, and the loader's own record has not got to it. A module the exe does
           not import at all (kfirst < 0) is not covered by the first argument, so it is left
           alone; each half alone is enough to leave a module running. */
        if (kfirst < 0 || kfirst <= kme || to_loader_entered(m)) {
            tagpu_logf("takeover: %s is %s, and the loader has already called its entry point "
                       "or may have -- it is left running, and the comparison at the first "
                       "DirectDraw call is what answers for this launch", base, what);
            continue;
        }
        /* A LIVE CALLBACK ARRAY IS A MODULE TO LEAVE RUNNING, NOT ONE TO MAKE INERT. The entry
           point this would overwrite is the module's C runtime start-up, so an inert one never
           allocates its TLS index -- and the loader still calls its DLL_THREAD_ATTACH callback on
           every thread the game creates, which would then index another module's TLS block. The
           launch refuses at the first DirectDraw call instead, where TADR's own writes are what
           it is refused over (part 3): a refusal is better than a wrong read per thread. */
        if (to_tls_callbacks(m)) {
            tagpu_logf("takeover: %s is %s and carries TLS callbacks the loader calls whatever "
                       "its entry point holds -- it is left running, and the comparison at the "
                       "first DirectDraw call is what answers for this launch", base, what);
            continue;
        }
        if (to_inert(m))
            tagpu_logf("takeover: %s is %s -- its entry point was made inert before the loader "
                       "called it, so none of its own code runs", base, what);
        else
            tagpu_logf("takeover: %s is %s and its entry point could not be made inert "
                       "(error %lu) -- its code runs", base, what, GetLastError());
    }
}



/* ---- the exe file read by virtual address: the reference part 2 compares with -------------
   The same file, opened the same way, that part 3 compares the whole image against: one
   reference for the whole takeover rather than two that could disagree. Open, read every site,
   close -- the mapping is not kept, so nothing of ours holds the player's exe open. */

static HANDLE s_refFile = INVALID_HANDLE_VALUE, s_refMap;
static const unsigned char* s_refView;
static DWORD s_refSize;
static const IMAGE_SECTION_HEADER* s_refSec;
static WORD s_refNsec;
static DWORD s_refBase;

int tagpu_takeover_file_open(void)
{
    wchar_t path[MAX_PATH];
    const IMAGE_DOS_HEADER* dos;
    const IMAGE_NT_HEADERS32* nt;
    DWORD first;
    if (s_refView) return 1;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 0;
    /* WITHOUT FILE_SHARE_WRITE, the way to_file_ask opens it: nothing may shorten the file under
       the view while it is read. A writer holding the exe means no reference at all, and every
       site of the table falls back to the stock bytes and says so. */
    s_refFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_refFile == INVALID_HANDLE_VALUE) return 0;
    s_refSize = GetFileSize(s_refFile, NULL);
    if (s_refSize == INVALID_FILE_SIZE || s_refSize < sizeof *dos || s_refSize > TO_MAX_FILE) {
        tagpu_takeover_file_close();
        return 0;
    }
    s_refMap = CreateFileMappingW(s_refFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (s_refMap) s_refView = (const unsigned char*)MapViewOfFile(s_refMap, FILE_MAP_READ, 0, 0, 0);
    if (!s_refView) { tagpu_takeover_file_close(); return 0; }
    dos = (const IMAGE_DOS_HEADER*)to_at(s_refView, s_refSize, 0, sizeof *dos);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) goto no;
    nt = (const IMAGE_NT_HEADERS32*)to_at(s_refView, s_refSize, (DWORD)dos->e_lfanew, sizeof *nt);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) goto no;
    /* The site table names absolute addresses, so the file can only be the reference for them
       when the exe is loaded at the base its file asks for -- as part 3 also requires. */
    if (nt->OptionalHeader.ImageBase != (DWORD)(size_t)GetModuleHandleW(NULL)) goto no;
    s_refBase = nt->OptionalHeader.ImageBase;
    s_refNsec = nt->FileHeader.NumberOfSections;
    first = (DWORD)((const unsigned char*)IMAGE_FIRST_SECTION(nt) - s_refView);
    if (!to_at(s_refView, s_refSize, first, (DWORD)s_refNsec * sizeof *s_refSec)) goto no;
    s_refSec = (const IMAGE_SECTION_HEADER*)(s_refView + first);
    return 1;
no:
    tagpu_takeover_file_close();
    return 0;
}

int tagpu_takeover_file_at(unsigned int va, void* out, int n)
{
    DWORD rva, i;
    if (!s_refView || !s_refSec || n <= 0 || va < s_refBase) return 0;
    rva = va - s_refBase;
    for (i = 0; i < s_refNsec; i++) {
        DWORD len = s_refSec[i].SizeOfRawData;
        if (rva < s_refSec[i].VirtualAddress) continue;
        if (rva - s_refSec[i].VirtualAddress >= len) continue;
        if (len - (rva - s_refSec[i].VirtualAddress) < (DWORD)n) return 0;
        if (!to_at(s_refView, s_refSize,
                   s_refSec[i].PointerToRawData + (rva - s_refSec[i].VirtualAddress), (DWORD)n))
            return 0;
        memcpy(out, s_refView + s_refSec[i].PointerToRawData +
                    (rva - s_refSec[i].VirtualAddress), (size_t)n);
        return 1;
    }
    return 0;
}

void tagpu_takeover_file_close(void)
{
    if (s_refView) UnmapViewOfFile(s_refView);
    if (s_refMap) CloseHandle(s_refMap);
    if (s_refFile != INVALID_HANDLE_VALUE) CloseHandle(s_refFile);
    s_refView = NULL;
    s_refMap = NULL;
    s_refFile = INVALID_HANDLE_VALUE;
    s_refSec = NULL;
}

/* ---- 1d. the exe imports TADR, so TADR's DllMain is what loads Impure -------------------
   The 3.9.02 exe imports TDRAW, Escalation's imports TAESC, and neither imports DDRAW: the
   loader never loads Impure from the exe, TADR's DllMain does with LoadLibraryA("ddraw.dll"),
   and this runs nested inside a DllMain that has done nothing but write one line to its own
   log and load dplayx.dll (DISASSEMBLED in two builds; MEASURED: the exe's code is still
   byte-identical to its file here). The fourth way in is answered like the other three -- THE
   REST OF THAT DllMain DOES NOT RUN -- and not by putting bytes back afterwards.

   HOW, in one sentence: the saved return address of that LoadLibrary call is replaced with a
   stub that returns TRUE out of TADR's DllMain. Nothing of TADR's code is written; the only
   write outside Impure is one dword of this thread's own stack, plus the exe's one import slot
   below.

   EVERY INPUT IS VERIFIED AT RUN TIME, none inferred from a build:
     * the frame is found by walking this thread's stack for a word that is a return address
       inside a game-folder module whose FILE carries TADR's marker, with a call in front of it,
       and whose NEXT word points at a string naming Impure's own module file -- LoadLibrary is
       stdcall, so its argument is still one word above its return address;
     * DllMain's own frame is the word above that which is a return address into the same
       module and is followed by `hinstDLL == that module's base` and `fdwReason ==
       DLL_PROCESS_ATTACH`. Those two arguments are what make the frame DllMain's rather than
       something that looks like it;
     * `ebp` is that word minus four, and the saved `ebp` it points at must itself be a stack
       address further up the stack;
     * the SEH registration to put back is FOUND, by walking fs:0 for the innermost record that
       lies inside the frame and whose Next lies outside it -- not assumed to be at ebp-0xC,
       which is only where the two builds read put it;
     * the caller must not pop the arguments itself (`add esp, imm8` at its return address),
       since then DllMain would be cdecl and the stub's `ret 0Ch` would move the caller's stack.
   ANY of them failing means NOTHING is written and the launch refuses at the first DirectDraw
   call exactly as it does today (part 3). A build this cannot read is left no worse than it is.

   WHAT IS LEFT BEHIND, and why it is little: TADR has started no thread yet (MEASURED: one
   thread, the exe's own, at this point; three at the first DirectDraw call when it is allowed to
   finish), it has subclassed no window, and its module's C runtime has already run -- the PE
   entry point IS the CRT's start-up and it calls DllMain after itself -- so the module is not
   left half-initialised in the way an inert entry point would leave it. Its exports must still
   never be called, which is what nest_take_imports is for. */

typedef struct {
    DWORD*  at;                  /* the stack slot holding the LoadLibrary return address */
    DWORD   ebp;                 /* TADR's DllMain frame pointer                          */
    DWORD   seh;                 /* what fs:0 must be put back to; 0 = leave it alone      */
    /* THE REGISTERS THE FRAME'S OWN EPILOGUE WOULD RESTORE, read out of its prologue. A stdcall
       callee owns ebx, esi, edi and ebp for its caller, so a return that skips its `pop`s hands
       the caller the callee's working values -- silently, and only on a build whose caller keeps
       something live in one of them. `reg` is the register number (3 ebx, 6 esi, 7 edi) and
       `slot` the stack address the prologue pushed it to. */
    unsigned char reg[3];
    DWORD   slot[3];
    int     nreg;
    HMODULE mod;
    char    name[64];
} NEST;

/* The top of the running thread's stack, so a walk cannot run off the end of it. */

static const unsigned char* nest_top(const void* here)
{
    MEMORY_BASIC_INFORMATION mbi, tip;
    /* NT_TIB.StackBase, one past the stack's highest usable byte. A VirtualQuery region stops at
       the first page whose state or protection differs, which can be below the frames wanted
       here; the TIB's value cannot. It is taken only when its last byte is committed in the same
       allocation `here` lies in -- the region, which is never too high, otherwise. */
    const unsigned char* top = (const unsigned char*)(size_t)nest_fs(4);
    if (!VirtualQuery(here, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return NULL;
    if (top > (const unsigned char*)here &&
        VirtualQuery(top - 1, &tip, sizeof tip) && tip.State == MEM_COMMIT &&
        tip.AllocationBase == mbi.AllocationBase)
        return top;
    return (const unsigned char*)mbi.BaseAddress + mbi.RegionSize;
}

/* Whether the bytes in front of `ret` are a call of any form. The form does not have to name
   its target: what names the target here is the argument on the stack, never the instruction
   (part 3 says why an address that merely sits in the bytes is no evidence). */
static int nest_call_before(DWORD ret, DWORD base)
{
    const unsigned char* p = (const unsigned char*)(size_t)ret;
    if (ret - base >= 6 && p[-6] == 0xFF && p[-5] == 0x15) return 1;             /* call [imm32] */
    if (ret - base >= 5 && p[-5] == 0xE8) return 1;                             /* call rel32   */
    /* call [r+d8] -- ModRM 0x54 takes a SIB byte, so that encoding is four bytes and does not
       end here. */
    if (ret - base >= 3 && p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50 && p[-2] != 0x54) return 1;
    if (ret - base >= 4 && p[-4] == 0xFF && p[-3] == 0x54) return 1;             /* call [r+r*s+d8] */
    if (ret - base >= 2 && p[-2] == 0xFF &&
        ((p[-1] & 0xF8) == 0xD0 || (p[-1] >= 0x10 && p[-1] <= 0x17))) return 1;  /* call r, [r]  */
    return 0;
}

/* Whether `s` points at a string naming Impure's own module file, ANSI or wide. The string is
   read through the module it lives in -- GetModuleHandleEx first, then bounded by that module's
   image -- so a stack word that happens to look like a pointer is never followed. */
static int nest_names_us(DWORD s)
{
    HMODULE owner = NULL;
    const IMAGE_NT_HEADERS32* nt;
    wchar_t self[MAX_PATH];
    char mine[64];
    DWORD room;
    const char* a;
    const wchar_t* w;
    DWORD i;
    if (!s || !g_ddraw_module) return 0;
    if (!GetModuleFileNameW(g_ddraw_module, self, MAX_PATH)) return 0;
    to_basename(self, mine, sizeof mine);
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(size_t)s, &owner) || !owner) return 0;
    nt = to_nt(owner);
    if (!nt) return 0;
    if (s < (DWORD)(size_t)owner) return 0;
    room = nt->OptionalHeader.SizeOfImage - (s - (DWORD)(size_t)owner);
    if (room > MAX_PATH) room = MAX_PATH;
    a = (const char*)(size_t)s;
    for (i = 0; i < room && a[i]; i++) ;
    if (i < room) {                       /* NUL-terminated inside the module: ANSI */
        const char* base = a;
        for (; *a; a++) if (*a == '\\' || *a == '/') base = a + 1;
        if (!_stricmp(base, mine)) return 1;
    }
    w = (const wchar_t*)(size_t)s;
    room /= sizeof(wchar_t);
    for (i = 0; i < room && w[i]; i++) ;
    if (i < room) {
        wchar_t wide[MAX_PATH];
        char narrow[64];
        _snwprintf(wide, MAX_PATH - 1, L"%s", w);
        wide[MAX_PATH - 1] = 0;
        to_basename(wide, narrow, sizeof narrow);
        if (!_stricmp(narrow, mine)) return 1;
    }
    return 0;
}

/* WHAT fs:0 MUST HOLD WHEN THE FRAME IS GONE, derived rather than guessed: the first record of
   the chain that is not below `ebp`. Any record below it belongs to this frame or to a frame
   deeper than it, and every one of those is unwound by the time the stub returns; the first one
   at or above `ebp` is what fs:0 held when the frame was entered, which is what its own epilogue
   would leave. That holds whether or not the frame registered a record itself, which a window
   below ebp cannot tell apart -- a `try` past 0x40 bytes of locals sits outside one, and reading
   "not found" as "registered none" leaves fs:0 pointing into dead stack.

   0 means "already correct": fs:0 is at or above the frame, so the stub leaves it alone. Every
   record is read inside this thread's own stack ([StackLimit, StackBase), the TIB's own bounds),
   and the walk is capped, so a corrupt chain ends the search instead of following it. */
static DWORD nest_seh(DWORD ebp, const unsigned char* top)
{
    DWORD at = nest_fs(0), lim = nest_fs(8);        /* fs:[0] the chain, fs:[8] StackLimit */
    int hops;
    if (!lim || (const unsigned char*)(size_t)lim >= top) return 0;
    for (hops = 0; hops < 64; hops++) {
        const DWORD* r;
        if (at >= ebp) return 0;                    /* nothing below the frame registered one */
        if (at < lim || (const unsigned char*)(size_t)at + 8 > top) return 0;
        r = (const DWORD*)(size_t)at;
        if (r[0] >= ebp) return r[0];               /* the first record the frame did not own */
        if (r[0] <= at) return 0;                   /* the chain must climb */
        at = r[0];
    }
    return 0;
}

/* Where a call goes, for the one call that entered a frame: `E8` names its target outright and
   `FF 15` names a pointer inside the module to read it from. Anything else is refused rather
   than guessed, since this decides which bytes are read as a prologue. */
static DWORD nest_call_target(DWORD ret, DWORD base, DWORD size)
{
    const unsigned char* p = (const unsigned char*)(size_t)ret;
    DWORD t = 0;
    if (ret - base >= 5 && p[-5] == 0xE8)
        t = ret + *(const DWORD*)(p - 4);
    else if (ret - base >= 6 && p[-6] == 0xFF && p[-5] == 0x15) {
        DWORD slot = *(const DWORD*)(p - 4);
        if (slot - base > size - sizeof(DWORD)) return 0;
        t = *(const DWORD*)(size_t)slot;
    }
    return (t - base < size) ? t : 0;
}

/* THE EPILOGUE THE FUNCTION WOULD HAVE RUN, read out of its prologue. `push ebp; mov ebp,esp`
   confirms the frame pointer the stub unwinds to -- guessing it from a stack word that merely
   looks like an address is what this replaces -- and each `push` of a callee-saved register
   after it is recorded with the address it pushed to, so the stub can put that register back
   from the frame. Stack adjustments are followed because a push before one and a push after it
   land at different offsets. Anything the walk does not know ends it, and a prologue that does
   not start with a frame pointer refuses the pass. */
static int nest_prologue(DWORD entry, DWORD base, DWORD size, DWORD ebp, NEST* n)
{
    const unsigned char* c = (const unsigned char*)(size_t)entry;
    DWORD room = size - (entry - base), i = 0;
    int off = 0, k;
    n->nreg = 0;
    if (room < 3 || entry - base >= size) return 0;
    if (c[0] != 0x55) return 0;                                     /* push ebp             */
    if (c[1] == 0x8B && c[2] == 0xEC) i = 3;                        /* mov ebp,esp          */
    else if (c[1] == 0x89 && c[2] == 0xE5) i = 3;
    else return 0;
    for (k = 0; k < 16 && i + 1 < room; k++) {
        if (c[i] == 0x53 || c[i] == 0x56 || c[i] == 0x57) {         /* push ebx/esi/edi     */
            off -= 4;
            if (n->nreg == 3) return 0;                             /* three is all there is */
            n->reg[n->nreg] = (unsigned char)(c[i] - 0x50);
            n->slot[n->nreg] = ebp + (DWORD)(int)off;
            n->nreg++;
            i++;
        } else if (c[i] == 0x83 && i + 2 < room && c[i + 1] == 0xEC) {
            off -= (int)(unsigned char)c[i + 2]; i += 3;            /* sub esp,imm8         */
        } else if (c[i] == 0x83 && i + 2 < room && c[i + 1] == 0xC4) {
            off += (int)(signed char)c[i + 2]; i += 3;              /* add esp,imm8 (signed) */
        } else if (c[i] == 0x81 && i + 5 < room && c[i + 1] == 0xEC) {
            off -= (int)*(const DWORD*)(c + i + 2); i += 6;         /* sub esp,imm32        */
        } else if (c[i] == 0x81 && i + 5 < room && c[i + 1] == 0xC4) {
            off += (int)*(const DWORD*)(c + i + 2); i += 6;         /* add esp,imm32        */
        } else {
            break;                                                  /* the body starts here */
        }
        if (off > 0 || off < -0x10000) return 0;    /* a prologue only ever makes room */
    }
    return 1;
}

static int nest_find(NEST* n, const wchar_t* game, const char** why)
{
    DWORD* p;
    const unsigned char* top;
    volatile DWORD here = 0;
    HMODULE m = NULL;
    wchar_t path[MAX_PATH];
    struct { DWORD base, size; HMODULE h; char name[64]; } tadr[8];
    int ntadr = 0, i;

    memset(n, 0, sizeof *n);
    *why = "the stack could not be read";
    top = nest_top((const void*)&here);
    if (!top) return 0;

    /* Which modules of the game folder are TADR's -- asked of the FILE, as everywhere else. */
    while ((m = to_next_local(m, game, path))) {
        if (ntadr == 8) {
            tagpu_logf("takeover: more than 8 modules of the game folder carry TADR's marker -- "
                       "the ones past the eighth are not looked for on the stack");
            break;
        }
        const char* what = NULL;
        const IMAGE_NT_HEADERS32* nt = to_nt(m);
        if (!nt || !to_file_ask(path, to_ask_tadr, &what)) continue;
        tadr[ntadr].base = (DWORD)(size_t)m;
        tadr[ntadr].size = nt->OptionalHeader.SizeOfImage;
        tadr[ntadr].h = m;
        to_basename(path, tadr[ntadr].name, sizeof tadr[ntadr].name);
        ntadr++;
    }
    *why = "no module of the game folder that carries TADR's marker is loaded";
    if (!ntadr) return 0;

    /* The LoadLibrary call that loaded us: the innermost return address into one of them whose
       argument -- one word above it, LoadLibrary being stdcall -- names Impure's own file. */
    *why = "no LoadLibrary of Impure's own file was found on the stack inside a TADR module";
    for (p = (DWORD*)(((size_t)&here + 3) & ~(size_t)3);
         (const unsigned char*)(p + 1) <= top; p++) {
        for (i = 0; i < ntadr; i++)
            if (*p >= tadr[i].base && *p - tadr[i].base < tadr[i].size &&
                nest_call_before(*p, tadr[i].base) &&
                (const unsigned char*)(p + 2) <= top && nest_names_us(p[1])) {
                n->at = p;
                n->mod = tadr[i].h;
                _snprintf(n->name, sizeof n->name - 1, "%s", tadr[i].name);
                break;
            }
        if (n->at) break;
    }
    if (!n->at) return 0;

    /* DllMain's own frame: a return address into the same module, followed by its first two
       arguments -- the module's own base, and DLL_PROCESS_ATTACH -- whose caller does NOT pop
       those arguments itself.

       THE OUTERMOST SUCH FRAME, and the popping test is what makes that the right one. A module
       built with the MSVC runtime puts two frames on the stack carrying the loader's argument
       triple: the entry point calls _DllMainCRTStartup CDECL (`add esp,0Ch` at its return), and
       that in turn calls the module's own DllMain STDCALL (DISASSEMBLED, TADR dev-dcff5dd and
       v2026.8.6: entry 0x…9C0 `push [ebp+10]/[ebp+0C]/[ebp+08]; call; add esp,0Ch; ret 0Ch`).
       Unwinding to the cdecl one would leave twelve bytes on its caller's stack and skip the C
       runtime's own exit as well; unwinding to the innermost match would take any helper the
       module happens to call with the same two values in the same places. The outermost frame
       whose caller leaves the arguments alone is DllMain itself. The entry point above it is
       called from ntdll, so its return address is not in the module and it never matches. */
    *why = "the DllMain frame of that module is not on the stack above the call";
    {
        DWORD base = (DWORD)(size_t)n->mod;
        const IMAGE_NT_HEADERS32* nt = to_nt(n->mod);
        DWORD size = nt ? nt->OptionalHeader.SizeOfImage : 0;
        DWORD* found = NULL;
        int popped = 0;
        if (!nt) return 0;
        for (p = n->at + 1; (const unsigned char*)(p + 3) <= top; p++) {
            const unsigned char* r;
            if (*p - base >= size || !nest_call_before(*p, base)) continue;
            if (p[1] != base || p[2] != DLL_PROCESS_ATTACH) continue;
            if (*p - base > size - 4) continue;
            r = (const unsigned char*)(size_t)*p;
            if ((r[0] == 0x83 && r[1] == 0xC4) || (r[0] == 0x81 && r[1] == 0xC4) ||
                (r[0] == 0x8D && r[1] == 0x64 && r[2] == 0x24)) {
                popped++;           /* cdecl: the stub's `ret 0Ch` would move its caller's stack */
                continue;
            }
            found = p;
        }
        if (!found) {
            if (popped)
                *why = "every DllMain frame of that module on the stack is called with the "
                       "arguments popped by its caller";
            return 0;
        }
        n->ebp = (DWORD)(size_t)(found - 1);

        /* ITS PROLOGUE, NOT A GUESS ABOUT IT. The call that entered the frame names the function,
           the function's first instructions name its frame pointer and every register it owes its
           caller, and a frame this cannot read is one the pass leaves alone. */
        *why = "the call that entered that DllMain does not name it";
        {
            DWORD entry = nest_call_target(*found, base, size);
            if (!entry) return 0;
            *why = "that module's DllMain does not open with a frame pointer this can unwind";
            if (!nest_prologue(entry, base, size, n->ebp, n)) return 0;
        }

        /* The saved ebp an ebp-framed function pushed points further up its own stack. */
        *why = "the DllMain frame does not carry a frame pointer this can unwind";
        {
            DWORD saved = *(const DWORD*)(size_t)n->ebp;
            int i;
            if (saved <= n->ebp || (const unsigned char*)(size_t)saved + 4 > top) return 0;
            for (i = 0; i < n->nreg; i++)
                if (n->slot[i] >= n->ebp ||
                    (const unsigned char*)(size_t)n->slot[i] + 4 > top ||
                    n->slot[i] < (DWORD)(size_t)n->at)
                    return 0;       /* a saved register must lie in the frame, above the call */
        }
    }
    n->seh = nest_seh(n->ebp, top);
    *why = NULL;
    return 1;
}

/* `mov eax,1` / the frame's own registers back / put fs:0 back / `mov esp,ebp` / `pop ebp` /
   `ret 0Ch`: TADR's DllMain returns TRUE to the loader having done nothing more, and its caller
   gets back every register a stdcall callee owes it. The immediates are this frame's, read above
   and valid for the one return this stub serves; each register is loaded from the stack address
   its own prologue pushed it to, absolutely, so the stub depends on no register it is handed. */
static void* nest_stub(const NEST* n)
{
    unsigned char code[64];
    int k = 0, i;
    DWORD old;
    void* page = VirtualAlloc(NULL, sizeof code, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!page) return NULL;
    code[k++] = 0xB8; *(DWORD*)(code + k) = 1; k += 4;              /* mov eax,1        */
    for (i = 0; i < n->nreg; i++) {                                 /* mov <reg>,[slot] */
        code[k++] = 0x8B;
        code[k++] = (unsigned char)(0x05 | (n->reg[i] << 3));
        *(DWORD*)(code + k) = n->slot[i]; k += 4;
    }
    if (n->seh) {
        code[k++] = 0xB9; *(DWORD*)(code + k) = n->seh; k += 4;     /* mov ecx,<next>   */
        code[k++] = 0x64; code[k++] = 0x89; code[k++] = 0x0D;       /* mov fs:0,ecx     */
        *(DWORD*)(code + k) = 0; k += 4;
    }
    code[k++] = 0xBC; *(DWORD*)(code + k) = n->ebp; k += 4;         /* mov esp,<ebp>    */
    code[k++] = 0x5D;                                               /* pop ebp          */
    code[k++] = 0xC2; code[k++] = 0x0C; code[k++] = 0x00;           /* ret 0Ch          */
    memcpy(page, code, (size_t)k);
    if (!VirtualProtect(page, sizeof code, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(page, 0, MEM_RELEASE);
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), page, sizeof code);
    return page;
}

/* The exe's imports from the module whose start-up was stopped: its exports must never be
   called now. All of a descriptor's slots or none, and only where Impure exports every name the
   descriptor imports -- which is one name, DirectDrawCreate, in both exes that take this route
   (DISASSEMBLED: objdump -p). A descriptor left alone is what part 3 refuses over. */
static void nest_take_imports(HMODULE mod, const char* name)
{
    BYTE* exe = (BYTE*)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS32* nt = to_nt((HMODULE)exe);
    const IMAGE_NT_HEADERS32* mnt = to_nt(mod);
    const IMAGE_DATA_DIRECTORY* dd;
    DWORD image, at, end, mbase = (DWORD)(size_t)mod;
    if (!exe || !nt || !mnt || !g_ddraw_module) return;
    image = nt->OptionalHeader.SizeOfImage;
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
    dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd->VirtualAddress || dd->VirtualAddress >= image) return;
    end = image - (DWORD)sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (at = dd->VirtualAddress; at <= end; at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        const IMAGE_IMPORT_DESCRIPTOR* d = (const IMAGE_IMPORT_DESCRIPTOR*)(exe + at);
        DWORD names, slots, k, n = 0;
        FARPROC ours[16];
        if (!d->FirstThunk) break;
        names = d->OriginalFirstThunk;
        slots = d->FirstThunk;
        if (!slots || slots >= image || slots + sizeof(DWORD) > image) continue;
        /* ANY of its slots, not slot 0: a slot the loader could not bind holds the file's value,
           and reading only the first would then miss the descriptor -- the trap to_descriptor_of
           names. */
        {
            DWORD j, leads = 0;
            for (j = 0; (j + 1) * sizeof(DWORD) <= image - slots; j++) {
                DWORD t = ((const DWORD*)(exe + slots))[j];
                if (!t) break;
                if (t - mbase < mnt->OptionalHeader.SizeOfImage) { leads = 1; break; }
            }
            if (!leads) continue;
        }
        if (!names || names >= image) {
            tagpu_logf("takeover: the exe's imports from %s cannot be named -- it has no name "
                       "table for them, and the comparison at the first DirectDraw call is what "
                       "answers for this launch", name);
            continue;
        }
        for (k = 0;; k++) {
            DWORD t;
            const IMAGE_IMPORT_BY_NAME* by;
            if (names >= image || slots >= image ||
                (k + 1) * sizeof(DWORD) > image - names ||
                (k + 1) * sizeof(DWORD) > image - slots) { n = 0; break; }
            t = ((const DWORD*)(exe + names))[k];
            if (!t) break;
            /* ALL OF A DESCRIPTOR OR NONE OF IT: a descriptor with more imports than there is
               room to answer must be left whole, or the slots past the room would still hold the
               module's addresses while the log claimed otherwise. */
            if (k >= sizeof ours / sizeof ours[0]) {
                tagpu_logf("takeover: the exe imports more than %u names from %s -- the "
                           "descriptor is left alone", (unsigned)(sizeof ours / sizeof ours[0]),
                           name);
                n = 0;
                break;
            }
            if (t & IMAGE_ORDINAL_FLAG32 || t + sizeof *by >= image) {
                tagpu_logf("takeover: the exe imports slot %lu from %s by ordinal, which Impure "
                           "cannot answer by name -- the descriptor is left alone", k, name);
                n = 0;
                break;
            }
            by = (const IMAGE_IMPORT_BY_NAME*)(exe + t);
            /* The name has to END inside the image, since GetProcAddress reads to its NUL. */
            {
                DWORD room = image - (t + (DWORD)sizeof *by), i;
                if (room > 256) room = 256;
                for (i = 0; i < room && by->Name[i]; i++) ;
                if (i >= room) {
                    tagpu_logf("takeover: an imported name from %s runs to the end of the exe's "
                               "image -- the descriptor is left alone", name);
                    n = 0;
                    break;
                }
            }
            ours[k] = GetProcAddress(g_ddraw_module, (const char*)by->Name);
            if (!ours[k]) {
                tagpu_logf("takeover: the exe imports %.32s from %s and Impure does not export "
                           "it -- the descriptor is left alone", (const char*)by->Name, name);
                n = 0;
                break;
            }
            n++;
        }
        if (!n) continue;
        /* The slots are about to hold addresses of Impure's code for the life of the process:
           PIN the module first, exactly as 1c does. */
        {
            HMODULE pinned = NULL;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                    (LPCWSTR)(void*)ours[0], &pinned)) {
                tagpu_logf("takeover: Impure's own module could not be pinned (error %lu) -- "
                           "the exe's imports from %s are left alone", GetLastError(), name);
                continue;
            }
        }
        {
            DWORD* slot = (DWORD*)(exe + slots), old;
            if (!VirtualProtect(slot, n * sizeof(DWORD), PAGE_READWRITE, &old)) {
                tagpu_logf("takeover: the exe's import slots from %s could not be written "
                           "(error %lu)", name, GetLastError());
                continue;
            }
            for (k = 0; k < n; k++) slot[k] = (DWORD)(size_t)ours[k];
            VirtualProtect(slot, n * sizeof(DWORD), old, &old);
        }
        tagpu_logf("takeover: the exe's %lu import(s) from %s now lead into Impure's own "
                   "exports, so nothing of that module is ever called", n, name);
    }
}

/* From DllMain, only where a module of the game folder started before Impure. Logs what it did
   either way; writes nothing at all unless every condition above holds. */
static HMODULE tagpu_takeover_nested_init(const wchar_t* game, int* kme)
{
    NEST n;
    const char* why = NULL;
    void* stub;
    const BYTE* exe = (const BYTE*)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS32* xnt = to_nt((HMODULE)exe);
    if (!nest_find(&n, game, &why)) {
        tagpu_logf("takeover: the start-up that loaded Impure was not stopped -- %s; the "
                   "comparison at the first DirectDraw call is what answers for this launch",
                   why ? why : "no reason");
        return NULL;
    }
    stub = nest_stub(&n);
    if (!stub) {
        tagpu_logf("takeover: %s's start-up was not stopped -- no page for the return stub "
                   "(error %lu)", n.name, GetLastError());
        return NULL;
    }
    *n.at = (DWORD)(size_t)stub;
    tagpu_logf("takeover: %s loaded Impure from its own DllMain, and the rest of that DllMain "
               "does not run -- its return address now leaves it with TRUE, frame 0x%08lX, "
               "%d register(s) put back, fs:0 %s", n.name, (unsigned long)n.ebp, n.nreg,
               n.seh ? "put back to the record above the frame" : "left as it is (already above "
               "the frame)");

    /* ITS DESCRIPTOR INDEX IS TAKEN NOW, BEFORE THE SLOTS ARE REDIRECTED. Afterwards those slots
       lead into Impure, and a slot match would then name that descriptor as Impure's; the name the
       loader bound still names this module, but the index is taken while both halves agree. */
    if (kme) *kme = (xnt && exe)
        ? to_descriptor_of(exe, xnt->OptionalHeader.SizeOfImage, n.mod, NULL) : -1;

    /* THE LOADER STILL HAS ONE CALL LEFT: DLL_PROCESS_DETACH at exit, which this module's own
       code would answer by freeing what it never set up. The entry point is made inert for it --
       the loader has already entered it, so nothing is being taken away from a call in flight,
       and the only call that can reach it now is the detach. Its TLS callbacks are untouched
       either way: the loader ran them before this module's entry point, so its TLS index exists. */
    if (to_inert(n.mod))
        tagpu_logf("takeover: %s's entry point was made inert as well, so the loader's "
                   "DLL_PROCESS_DETACH call at exit runs none of its code either", n.name);
    else
        tagpu_logf("takeover: %s's entry point could not be made inert, so its own "
                   "DLL_PROCESS_DETACH cleanup runs at exit (error %lu)", n.name,
                   GetLastError());

    nest_take_imports(n.mod, n.name);
    return n.mod;
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
            if (names >= image || slots >= image ||
                (k + 1) * sizeof(DWORD) > image - names ||
                (k + 1) * sizeof(DWORD) > image - slots) {
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
   crash. On the routes where TADR loads Impure this is what stands behind 1d as well: the exe's
   own TDRAW/TAESC slots are the ones nest_take_imports redirects, and a descriptor it left alone
   -- because a name it could not answer, or one it could not read -- is refused here. */
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
        if (slots >= image) continue;
        /* a bound IAT keeps the null terminator the file's array ends with */
        for (k = 0; (k + 1) * sizeof(DWORD) <= image - slots; k++) {
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
