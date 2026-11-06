/* tagpu_modload.c -- Contract: tagpu_modload.h. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_modload.h"
#include "tagpu_modpak.h"
#include "tagpu_modcat.h"
#include "tagpu_pak.h"
#include "tagpu_settings.h"
#include "tagpu_detour.h"
#include "tagpu_log.h"
#include "zstd.h"

#define PACK_MAX (1u << 20)            /* patches.bin, decompressed: far more than it holds */
#define EXE_MAX (64u << 20)
#define IMPURE_UFO "impure-patch.ufo"  /* Impure's own menu screens, written at attach (tagpu_menu.c) */
#define ARCHIVE_CACHE "mods\\archives.cache"
#define ARCHIVES_MAX 64

/* The engine's sites, DISASSEMBLED 2026-11-05; modpack.py's MOUNT_SITES keeps every table off them. */
#define VA_MOUNTER   0x4BE0B0u          /* stdcall (name, flag), ret 8; only InitTAHPIAry 0x41D4C0 calls it */
#define VA_OPEN_CALL 0x4BB32Du          /* the path open 0x4BB2E0's loose attempt: call 0x4E4990 (fopen) */
#define VA_FOPEN     0x4E4990u
#define VA_FIND_CALL 0x4BC5D3u          /* the enumeration 0x4BC4B0's loose listing: call 0x4E7DD0 (_findfirst) */
#define VA_FINDFIRST 0x4E7DD0u
#define VA_OPTIONS_NAME 0x5098A3u       /* engine/process.md: 12 characters and a NUL */

static const unsigned char MOUNTER_STOLEN[6] = { 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00 };  /* sub esp, 0x104 */

static char s_id[33];
static char s_refusal[512];
static uint8_t* s_blob;
static MODPAK s_pak;
static MODIMG s_target;                 /* stock + the mod's XOR */
static const MODXOR* s_xor;
static int s_loader;
static MODIMG s_snapshot;
static int s_entered;
static const unsigned char* s_catalog;
static size_t s_ncatalog;

static void refuse(const char* fmt, ...)
{
    va_list ap;
    if (s_refusal[0]) return;
    va_start(ap, fmt);
    vsnprintf(s_refusal, sizeof s_refusal, fmt, ap);
    va_end(ap);
    tagpu_logf("modload: REFUSED %s: %s", s_id, s_refusal);
}

const char* tagpu_modload_id(void) { return s_id[0] ? s_id : NULL; }

/* ---- the files --------------------------------------------------------------------------------- */

static uint8_t* read_file_w(const wchar_t* path, size_t* n)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    uint8_t* b = NULL;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    if (size != INVALID_FILE_SIZE && size <= EXE_MAX && (b = malloc((size_t)size + 1))) {
        if (!ReadFile(h, b, size, &got, NULL) || got != size) { free(b); b = NULL; }
    }
    CloseHandle(h);
    *n = size;
    return b;
}

static int exists(const char* path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static int contains(const uint8_t* b, size_t n, const char* s)
{
    size_t k = strlen(s), i;
    for (i = 0; i + k <= n; i++) if (b[i] == (uint8_t)s[0] && !memcmp(b + i, s, k)) return 1;
    return 0;
}

static int load_pack(void)
{
    size_t npak = 0, n = 0, got;
    const unsigned char* pak = tagpu_pak_resource("MODPAK", &npak);
    const unsigned char* z = pak ? tagpu_pak_file(pak, npak, "patches.bin", &n) : NULL;
    s_catalog = pak ? tagpu_pak_file(pak, npak, "catalog.json", &s_ncatalog) : NULL;
    if (!z || !s_catalog || !(s_blob = malloc(PACK_MAX))) return 0;
    got = ZSTD_decompress(s_blob, PACK_MAX, z, n);
    return !ZSTD_isError(got) && tagpu_modpak_read(s_blob, got, &s_pak);
}

/* ---- the image --------------------------------------------------------------------------------- */

/* `img`'s bytes over the process's, the ranges of `mask` left as they are; how many changed. Every section
   is mapped at least as long as its raw bytes, by the PE loader's own rule. */
static int put_image(const MODIMG* img, const MODRANGE* mask, uint32_t nmask, int* first)
{
    int s, changed = 0;
    for (s = 0; s < img->nsec; s++) {
        const MODSEC* c = &img->sec[s];
        volatile uint8_t* mem = (volatile uint8_t*)(uintptr_t)c->va;
        DWORD old;
        uint32_t i, m = 0;
        if (!VirtualProtect((void*)(uintptr_t)c->va, c->len, PAGE_EXECUTE_READWRITE, &old)) return -1;
        for (i = 0; i < c->len; i++) {
            uint32_t va = c->va + i;
            while (m < nmask && va >= mask[m].va + mask[m].len) m++;
            if (m < nmask && va >= mask[m].va) continue;
            if (mem[i] != c->bytes[i]) {
                if (!changed && first) *first = (int)va;
                mem[i] = c->bytes[i];
                changed++;
            }
        }
        VirtualProtect((void*)(uintptr_t)c->va, c->len, old, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    return changed;
}

/* A loader mod's bytes, written over whatever is there as its loader's list would be. */
static int put_runs(const MODXOR* x, const MODIMG* img)
{
    const uint8_t* p = x->at;
    uint32_t r;
    for (r = 0; r < x->nruns; r++) {
        uint32_t va = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        uint32_t len = p[4] | (uint32_t)p[5] << 8;
        int s;
        for (s = 0; s < img->nsec; s++)
            if (va >= img->sec[s].va && va - img->sec[s].va < img->sec[s].len) break;
        /* tagpu_modimg_xor has already placed each run inside one section */
        if (s == img->nsec || !tagpu_detour_write(va, img->sec[s].bytes + (va - img->sec[s].va), (int)len)) return 0;
        p += 6 + len;
    }
    return 1;
}

void tagpu_modload_begin(void)
{
    char id[sizeof s_id], path[MAX_PATH];
    wchar_t exe_path[MAX_PATH], dir[MAX_PATH], *slash;
    uint8_t* exe = NULL, *dll;
    uint8_t key[32], hash[32];
    size_t nexe = 0, ndll = 0;
    const MODPATCH* mod;
    const MODKNOWN* known = NULL;
    int stock, i, changed, first = 0;

    if (!tagpu_settings_mod(id, sizeof id)) return;
    lstrcpynA(s_id, id, sizeof s_id);
    for (i = 0; id[i]; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '_' || id[i] == '-')) {
            refuse("that is not the name of a mod");
            return;
        }
    if (!load_pack()) { refuse("this build of Impure has no usable mod pack"); return; }
    if (!(mod = tagpu_modpak_mod(&s_pak, id))) { refuse("Impure does not know that mod"); return; }
    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH) || !(exe = read_file_w(exe_path, &nexe)) ||
        !tagpu_modpak_file_key(exe, nexe, 1, key)) {
        refuse("Total Annihilation's exe could not be read");
        goto done;
    }
    stock = !memcmp(key, s_pak.stock_key, 32);
    if (!stock && !(known = tagpu_modpak_known(&s_pak, 0, key))) {
        refuse("mods are not available because this exe is not one Impure knows");
        goto done;
    }
    /* a Patch Loader beside it is a known one: what it writes goes at the entry point, but an unknown one
       is no folder this was tested on */
    lstrcpyW(dir, exe_path);
    if ((slash = wcsrchr(dir, L'\\'))) slash[1] = 0;
    if (lstrlenW(dir) + 12 < MAX_PATH) {
        lstrcatW(dir, L"dplayx.dll");
        if ((dll = read_file_w(dir, &ndll))) {
            int is_loader = contains(dll, ndll, "[Settings]");
            uint8_t lkey[32];
            tagpu_modpak_file_key(dll, ndll, 0, lkey);
            free(dll);
            if (is_loader && !tagpu_modpak_known(&s_pak, 1, lkey)) {
                refuse("mods are not available because the Patch Loader in this folder is not one Impure knows");
                goto done;
            }
        }
    }
    snprintf(path, sizeof path, "mods\\%s\\%s.hpi", id, id);
    if (!exists(path)) { refuse("it is not installed: %s is missing", path); goto done; }
    snprintf(path, sizeof path, "mods\\%s\\%s.originals.hpi", id, id);
    if (!exists(path)) { refuse("it is not installed: %s is missing", path); goto done; }

    s_xor = &s_pak.xors[mod->xor_];
    s_loader = mod->loader;
    if (!tagpu_modimg_from_exe(&s_target, exe, nexe) ||
        (known && !tagpu_modimg_xor(&s_target, &s_pak.xors[known->xor_])) ||
        !tagpu_modimg_xor(&s_target, s_xor)) {
        refuse("this exe's sections do not fit the mod's patch");
        goto done;
    }
    tagpu_modimg_hash(&s_target, s_pak.mask, s_pak.nmask, hash);
    if (memcmp(hash, mod->hash, 32)) { refuse("the patched exe does not have the hash the mod names"); goto done; }

    if (s_loader) {
        tagpu_modimg_xor(&s_target, s_xor);        /* back to stock, for this one write */
        changed = put_image(&s_target, s_pak.mask, s_pak.nmask, &first);
        tagpu_modimg_xor(&s_target, s_xor);
    } else {
        changed = put_image(&s_target, s_pak.mask, s_pak.nmask, &first);
    }
    if (changed < 0) { refuse("the exe's image could not be written (%lu)", GetLastError()); goto done; }
    /* the options file the image names (0x5098A3): the engine and the icons read it beside the exe, and it is the
       player's once there, so it is only ever copied when absent */
    {
        char ini[14], from[MAX_PATH];
        int s, k;
        ini[0] = 0;
        for (s = 0; s < s_target.nsec; s++)
            if (VA_OPTIONS_NAME >= s_target.sec[s].va && VA_OPTIONS_NAME + sizeof ini <= s_target.sec[s].va + s_target.sec[s].len)
                memcpy(ini, s_target.sec[s].bytes + (VA_OPTIONS_NAME - s_target.sec[s].va), sizeof ini);
        ini[sizeof ini - 1] = 0;
        for (k = 0; ini[k] && ini[k] != '\\' && ini[k] != '/' && ini[k] != ':'; k++) {}
        if (ini[0] && !ini[k]) {
            snprintf(from, sizeof from, "mods\\%s\\%s", id, ini);
            if (CopyFileA(from, ini, TRUE)) tagpu_logf("modload: %s copied beside the exe", ini);
        }
    }
    tagpu_logf("modload: %s on %s exe: %s, %d B changed%s", id, stock ? "the stock" : "a known",
               s_loader ? "stock in place, its list goes after Impure's patches" : "its image in place", changed,
               changed ? "" : " (none)");
done:
    free(exe);
}

/* ---- the mount --------------------------------------------------------------------------------- */

typedef struct { char name[64]; unsigned long long size, date; uint8_t sha[32]; int original; } ARCHIVE;
static ARCHIVE s_arc[ARCHIVES_MAX];
static int s_narc, s_arcLoaded;
static int (__stdcall *s_mountReal)(const char*, int);
static int s_ours;

static void hex32(const uint8_t* h, char* out)
{
    int i;
    for (i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

/* mods\archives.cache: name, size, date, SHA-256 of each archive seen. Hashing them all takes seconds, and a
   file changed since has another size or date. A row that does not parse is only hashed again. */
static void load_archive_cache(void)
{
    char text[ARCHIVES_MAX * 160];
    DWORD got = 0;
    char* line, *next;
    HANDLE h = CreateFileA(ARCHIVE_CACHE, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    s_arcLoaded = 1;
    if (h == INVALID_HANDLE_VALUE) return;
    ReadFile(h, text, sizeof text - 1, &got, NULL);
    CloseHandle(h);
    text[got] = 0;
    for (line = text; line && s_narc < ARCHIVES_MAX; line = next) {
        ARCHIVE* a = &s_arc[s_narc];
        char hex[65];
        int i;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (sscanf(line, "%63[^\t]\t%llu\t%llu\t%64s", a->name, &a->size, &a->date, hex) != 4 || strlen(hex) != 64) continue;
        for (i = 0; i < 32; i++) {
            unsigned b;
            if (sscanf(hex + 2 * i, "%2x", &b) != 1) break;
            a->sha[i] = (uint8_t)b;
        }
        if (i < 32) continue;
        a->original = tagpu_modpak_is_original(&s_pak, a->sha);
        s_narc++;
    }
}

static void save_archive_cache(void)
{
    char tmp[] = ARCHIVE_CACHE ".new", hex[65];
    FILE* f = fopen(tmp, "wb");
    int i, ok = 1;
    if (!f) return;
    for (i = 0; i < s_narc; i++) {
        hex32(s_arc[i].sha, hex);
        ok = fprintf(f, "%s\t%llu\t%llu\t%s\n", s_arc[i].name, s_arc[i].size, s_arc[i].date, hex) > 0 && ok;
    }
    ok = !fclose(f) && ok;
    if (!ok || !MoveFileExA(tmp, ARCHIVE_CACHE, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
}

/* Whether this file is an original archive of the game, by its bytes: mods reuse and replace the names. */
static int is_original_archive(const char* name)
{
    static uint8_t buf[1u << 20];
    WIN32_FILE_ATTRIBUTE_DATA st;
    unsigned long long size, date;
    ARCHIVE* a = NULL;
    MODSHA sha;
    HANDLE h;
    DWORD got;
    int i;
    if (!GetFileAttributesExA(name, GetFileExInfoStandard, &st)) return 0;
    size = (unsigned long long)st.nFileSizeHigh << 32 | st.nFileSizeLow;
    date = (unsigned long long)st.ftLastWriteTime.dwHighDateTime << 32 | st.ftLastWriteTime.dwLowDateTime;
    if (!s_arcLoaded) load_archive_cache();
    for (i = 0; i < s_narc; i++)
        if (!lstrcmpiA(s_arc[i].name, name)) {
            a = &s_arc[i];
            if (a->size == size && a->date == date) return a->original;
            break;
        }
    if (!a) {
        if (s_narc == ARCHIVES_MAX) return 0;
        a = &s_arc[s_narc++];
        lstrcpynA(a->name, name, sizeof a->name);
    }
    h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    modsha_init(&sha);
    while (ReadFile(h, buf, sizeof buf, &got, NULL) && got) modsha_add(&sha, buf, got);
    CloseHandle(h);
    modsha_end(&sha, a->sha);
    a->size = size;
    a->date = date;
    a->original = tagpu_modpak_is_original(&s_pak, a->sha);
    save_archive_cache();
    return a->original;
}

static int __stdcall mount_filtered(const char* name, int flag)
{
    const char* base = name, *p;
    if (!s_ours) {
        char path[MAX_PATH];
        s_ours = 1;
        /* flag 1 as the mods' first patterns have it: the archive every path looks in first */
        snprintf(path, sizeof path, "mods\\%s\\%s.hpi", s_id, s_id);
        tagpu_logf("modload: mounted %s: %d", path, s_mountReal(path, 1));
        snprintf(path, sizeof path, "mods\\%s\\%s.originals.hpi", s_id, s_id);
        tagpu_logf("modload: mounted %s: %d", path, s_mountReal(path, 1));
    }
    for (p = name; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    if (lstrcmpiA(base, IMPURE_UFO)) {
        static char seen[ARCHIVES_MAX][64];
        static int nseen;
        const char* why = NULL;
        if (tagpu_modcat_hidden((const char*)s_catalog, s_ncatalog, s_id, base)) why = "the mod hides it";
        else if (!is_original_archive(name)) why = "it is no original archive of the game";
        if (why) {
            int i;
            for (i = 0; i < nseen && lstrcmpiA(seen[i], base); i++) {}
            if (i == nseen && nseen < ARCHIVES_MAX) {
                lstrcpynA(seen[nseen++], base, sizeof seen[0]);
                tagpu_logf("modload: not mounted %s: %s", base, why);
            }
            return 0;   /* as for one already mounted: *.HPI's ten-a-call count does not move (0x41D5D5) */
        }
    }
    return s_mountReal(name, flag);
}

/* ---- the loose step ---------------------------------------------------------------------------- */

/* The folders the engine reads through its file open, the list modpack.py's GAME_DIRS packs from. The rest is
   no game data: the saves, what a player keeps beside the game. */
int tagpu_modload_loose_closed(const char* path)
{
    static const char* const dirs[] = {
        "anims", "bitmaps", "camera", "camps", "cdplayer", "download", "features", "fonts", "gamedata", "guis", "maps",
        "objects3d", "palettes", "scripts", "sections", "sounds", "textures", "unitpics", "units", "ai", "weapons",
        "unitscript", "unitsb", "cursors"
    };
    char first[32];
    unsigned i, n = 0;
    if (!s_id[0] || !path) return 0;
    if (path[0] == '.' && (path[1] == '\\' || path[1] == '/')) path += 2;
    while (path[n] && path[n] != '\\' && path[n] != '/') {
        if (n >= sizeof first - 1) return 0;
        first[n] = path[n];
        n++;
    }
    if (!path[n]) return 0;     /* a bare name: no folder of game data */
    first[n] = 0;
    for (i = 0; i < sizeof dirs / sizeof *dirs; i++) if (!lstrcmpiA(first, dirs[i])) return 1;
    return 0;
}

static void note_closed(const char* path)
{
    static int noted;
    if (noted < 32 && exists(path)) {
        noted++;
        tagpu_logf("modload: the loose file %s is not read under a mod", path);
    }
}

static void* __cdecl open_filtered(const char* path, const char* mode)
{
    if (mode && mode[0] == 'r' && tagpu_modload_loose_closed(path)) {
        note_closed(path);
        return NULL;
    }
    return ((void* (__cdecl*)(const char*, const char*))VA_FOPEN)(path, mode);
}

static long __cdecl find_filtered(const char* spec, void* data)
{
    if (tagpu_modload_loose_closed(spec)) return -1;
    return ((long (__cdecl*)(const char*, void*))VA_FINDFIRST)(spec, data);
}

/* a call's rel32 moved to `to`, after checking it reaches `from` */
static int redirect_call(unsigned va, unsigned from, void* to)
{
    const unsigned char* p = (const unsigned char*)(uintptr_t)va;
    unsigned rel = (unsigned)(uintptr_t)to - (va + 5), was;
    memcpy(&was, p + 1, 4);
    if (p[0] != 0xE8 || was != from - (va + 5)) return 0;
    return tagpu_detour_write(va + 1, (const unsigned char*)&rel, 4);
}

static int install_mount(void)
{
    unsigned char* stub;
    unsigned rel;
    if (!tagpu_detour_bytes_ok(VA_MOUNTER, MOUNTER_STOLEN, sizeof MOUNTER_STOLEN) || !(stub = tagpu_detour_stub())) return 0;
    memcpy(stub, MOUNTER_STOLEN, sizeof MOUNTER_STOLEN);
    stub[sizeof MOUNTER_STOLEN] = 0xE9;
    rel = (VA_MOUNTER + sizeof MOUNTER_STOLEN) - ((unsigned)(uintptr_t)stub + sizeof MOUNTER_STOLEN + 5);
    memcpy(stub + sizeof MOUNTER_STOLEN + 1, &rel, 4);
    s_mountReal = (int (__stdcall*)(const char*, int))stub;
    return tagpu_detour_land(VA_MOUNTER, (const unsigned char*)mount_filtered, sizeof MOUNTER_STOLEN) &&
           redirect_call(VA_OPEN_CALL, VA_FOPEN, open_filtered) &&
           redirect_call(VA_FIND_CALL, VA_FINDFIRST, find_filtered);
}

/* ---- the entry point --------------------------------------------------------------------------- */

void* tagpu_modload_entry_target;

static void box_and_exit(void)
{
    char text[sizeof s_refusal + 200];
    snprintf(text, sizeof text, "The mod \"%s\" cannot start:\n\n%s.\n\nThe next start runs without it.", s_id, s_refusal);
    MessageBoxA(NULL, text, "Total Annihilation: Impure", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
    tagpu_settings_set_mod("");
    ExitProcess(1);
}

/* the exe's entry point, reached on its one thread before any of its code */
void tagpu_modload_entry_stage(void)
{
    int first = 0, changed;
    s_entered = 1;
    if (s_refusal[0]) box_and_exit();
    changed = put_image(&s_snapshot, NULL, 0, &first);
    if (changed < 0) {
        refuse("the exe's image could not be written (%lu)", GetLastError());
        box_and_exit();
    }
    /* our own jump is 5 of them, and the first */
    tagpu_logf("modload: at the entry point: %d B other DLLs wrote were put back", changed - 5);
    tagpu_modimg_free(&s_snapshot);
    tagpu_modimg_free(&s_target);
}

__attribute__((naked)) static void entry_stub(void)
{
    __asm__("pushal\n\tcall _tagpu_modload_entry_stage\n\tpopal\n\tjmp *_tagpu_modload_entry_target\n");
}

void tagpu_modload_end(void)
{
    HMODULE exe = GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
    const IMAGE_NT_HEADERS32* nt = (const IMAGE_NT_HEADERS32*)((const char*)exe + dos->e_lfanew);
    unsigned entry = (unsigned)(uintptr_t)exe + nt->OptionalHeader.AddressOfEntryPoint;
    unsigned char jump[5] = { 0xE9 };
    unsigned rel;
    int s;
    if (!s_id[0]) return;
    if (!s_refusal[0]) {
        if (s_loader && !put_runs(s_xor, &s_target)) refuse("the mod's patch could not be written (%lu)", GetLastError());
        if (!s_refusal[0] && !install_mount()) refuse("the engine's archive code is not what Impure expects");
        if (!s_refusal[0]) {
            /* the image as Impure's whole attach left it, so the entry point keeps Impure's patches and takes
               back only what came after */
            s_snapshot.nsec = s_target.nsec;
            for (s = 0; s < s_target.nsec; s++) {
                s_snapshot.sec[s] = s_target.sec[s];
                if (!(s_snapshot.sec[s].bytes = malloc(s_target.sec[s].len))) {
                    s_snapshot.sec[s].len = 0;
                    refuse("no memory for the exe's image");
                    break;
                }
                memcpy(s_snapshot.sec[s].bytes, (const void*)(uintptr_t)s_target.sec[s].va, s_target.sec[s].len);
            }
        }
    }
    tagpu_modload_entry_target = (void*)(uintptr_t)entry;
    rel = (unsigned)(uintptr_t)entry_stub - (entry + 5);
    memcpy(jump + 1, &rel, 4);
    if (!tagpu_detour_write(entry, jump, 5)) tagpu_logf("modload: the entry point could not be written (%lu)", GetLastError());
}

void tagpu_modload_check(void)
{
    if (!s_id[0] || s_entered) return;
    refuse("another DLL in this folder took over the exe's entry point");
    box_and_exit();
}
