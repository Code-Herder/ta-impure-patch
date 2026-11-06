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
#include "tagpu_refuse.h"
#include "tagpu_modinst.h"
#include "tagpu_profiles.h"
#include "tagpu_modfit.h"
#include "tagpu_hpi.h"
#include "zstd.h"

#define PACK_MAX (1u << 20)            /* patches.bin, decompressed: far more than it holds */
#define EXE_MAX (64u << 20)
#define IMPURE_UFO "impure-patch.ufo"  /* Impure's own menu screens, written at attach (tagpu_menu.c) */
#define ARCHIVE_CACHE "mods\\archives.cache"
#define ARCHIVES_MAX 64
#define ESSENTIAL_ARCHIVE "totala1.hpi"
#define MUTATORS_MAX 16
#define FOLDER_BASE "none"              /* the base of a mutator for the folder's own game: a profile's name, and a catalog's word */

/* The engine's sites, DISASSEMBLED 2026-11-05; modpack.py's MOUNT_SITES keeps every table off them. */
#define VA_MOUNTER   0x4BE0B0u          /* stdcall (name, flag), ret 8; only InitTAHPIAry 0x41D4C0 calls it */
#define VA_OPEN_CALL 0x4BB32Du          /* the path open 0x4BB2E0's loose attempt: call 0x4E4990 (fopen) */
#define VA_FOPEN     0x4E4990u
#define VA_FIND_CALL 0x4BC5D3u          /* the enumeration 0x4BC4B0's loose listing: call 0x4E7DD0 (_findfirst) */
#define VA_FINDFIRST 0x4E7DD0u
#define VA_MOUNT_PATH_MAX 0x100u        /* the smaller of the mounter's two path buffers */
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
static uint8_t* s_reference;
static size_t s_nreference;
static int s_entered;
static const unsigned char* s_catalog;
static size_t s_ncatalog;

/* The folder names the engine pushes when it reads game data, which a mod's image can rename (Escalation unitsE, TA Zero ZUnits,
   Mayhem guiM: engine/ui.md and engine/process.md, *The folder names a mod renames*). A renamed folder is still game data. */
static const uint32_t FOLDER_NAMES[] = {
    0x502820u, 0x503360u, 0x505FE4u, 0x503730u, 0x50925Cu, 0x5031BCu, 0x5031C8u,
    0x503920u, 0x503720u, 0x504734u, 0x5036E4u, 0x503740u, 0x502E30u, 0x502C7Cu, 0x50392Cu, 0x504B10u
};
#define FOLDER_NAME_MAX 24
static char s_folder[sizeof FOLDER_NAMES / sizeof *FOLDER_NAMES][FOLDER_NAME_MAX];

/* The three sites the mount and the loose step rest on, in the file's bytes: no mod's table touches them (modpack's
   MOUNT_SITES), so they hold on every exe of the game, known or not. */
static const uint8_t* image_at(const MODIMG* img, uint32_t va, uint32_t n)
{
    int s;
    for (s = 0; s < img->nsec; s++)
        if (va >= img->sec[s].va && n <= img->sec[s].len && va - img->sec[s].va <= img->sec[s].len - n)
            return img->sec[s].bytes + (va - img->sec[s].va);
    return NULL;
}

static int calls(const MODIMG* img, uint32_t va, uint32_t to)
{
    const uint8_t* p = image_at(img, va, 5);
    uint32_t rel = to - (va + 5);
    return p && p[0] == 0xE8 && !memcmp(p + 1, &rel, 4);
}

static int is_the_game(const MODIMG* img)
{
    const uint8_t* m = image_at(img, VA_MOUNTER, sizeof MOUNTER_STOLEN);
    return m && !memcmp(m, MOUNTER_STOLEN, sizeof MOUNTER_STOLEN) &&
           calls(img, VA_OPEN_CALL, VA_FOPEN) && calls(img, VA_FIND_CALL, VA_FINDFIRST);
}

static int s_keep_choice;
static uint8_t* read_file_w(const wchar_t* path, size_t* n);
static int exists(const char* path);

static void refuse(const char* fmt, ...)
{
    va_list ap;
    if (s_refusal[0]) return;
    va_start(ap, fmt);
    vsnprintf(s_refusal, sizeof s_refusal, fmt, ap);
    va_end(ap);
    tagpu_logf("modload: REFUSED %s: %s", s_id, s_refusal);
}

/* A folder another copy of the game is moving is no verdict on the player's choice, so that box keeps mod=. */
static void not_installed(const char* path)
{
    int locked = tagpu_modinst_locked();
    if (locked || exists(path)) {
        /* the second test: a swap that finished between the first test and the lock's release */
        s_keep_choice = 1;
        refuse("another copy of the game is installing a mod: start this one when it has finished");
    } else {
        refuse("it is not installed: %s is missing", path);
    }
}

const char* tagpu_modload_id(void) { return s_id[0] ? s_id : NULL; }

int tagpu_modload_is_game(void)
{
    wchar_t path[MAX_PATH];
    const wchar_t* base;
    uint8_t* exe;
    size_t n = 0;
    MODIMG img;
    int sites;

    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return 0;
    base = wcsrchr(path, L'\\');
    if (!lstrcmpiW(base ? base + 1 : path, L"TotalA.exe")) return 1;
    if (!(exe = read_file_w(path, &n))) return 0;
    memset(&img, 0, sizeof img);
    sites = tagpu_modimg_from_exe(&img, exe, n) && is_the_game(&img);
    tagpu_modimg_free(&img);
    free(exe);
    return sites;
}

const unsigned char* tagpu_modload_reference_file(size_t* n)
{
    *n = s_nreference;
    return s_reference;
}

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
    s_catalog = (const unsigned char*)tagpu_modinst_catalog(&s_ncatalog);
    if (!z || !s_catalog || !(s_blob = malloc(PACK_MAX))) return 0;
    got = ZSTD_decompress(s_blob, PACK_MAX, z, n);
    return !ZSTD_isError(got) && tagpu_modpak_read(s_blob, got, &s_pak);
}

/* ---- the image --------------------------------------------------------------------------------- */

/* `img`'s bytes over the process's, the ranges of `mask` left as they are; how many changed. `img` is only ever a file
   file_key recognised, and every section of those has its raw length within its page-rounded VirtualSize (MEASURED
   2026-11-05, the eight known exes), so each write is inside the section the loader mapped. */
static int put_image(const MODIMG* img, const MODRANGE* mask, uint32_t nmask)
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

static void note_catalog_id(void* ctx, const char* id);

typedef struct { char id[64][32]; int n; } IDS;
static int s_folderMutators;

/* whether the player has switched on any mutator of the folder's own game: only then does a launch without mod= detour the mounter */
static int folder_wants_mutators(void)
{
    IDS ids;
    size_t n = 0;
    const char* cat = tagpu_modinst_catalog(&n);
    int i, want = 0;
    ids.n = 0;
    if (!cat) return 0;
    tagpu_modcat_each_id(cat, n, note_catalog_id, &ids);
    for (i = 0; i < ids.n && !want; i++) {
        MODABOUT a;
        want = tagpu_modcat_about(cat, n, ids.id[i], &a) && a.is_mutator && !strcmp(a.base, FOLDER_BASE) &&
               tagpu_profiles_mutator_of(FOLDER_BASE, ids.id[i]) == 1;
    }
    return want;
}

void tagpu_modload_begin(void)
{
    char id[sizeof s_id], path[MAX_PATH], full[MAX_PATH];
    wchar_t exe_path[MAX_PATH], dir[MAX_PATH], *slash;
    uint8_t* exe = NULL, *dll;
    uint8_t key[32], hash[32];
    size_t nexe = 0, ndll = 0;
    const MODPATCH* mod;
    const MODKNOWN* known = NULL;
    int stock, i, changed;

    if (!tagpu_settings_mod(id, sizeof id)) {
        s_folderMutators = folder_wants_mutators();
        return;
    }
    /* Any exe that loads a ddraw.dll from this folder loads this one. A file with another name and none of the engine's sites
       is a tool, and the player's mod= is none of its business; TotalA.exe without them is a game no key knows, and refuses. */
    if (!GetModuleFileNameW(NULL, exe_path, MAX_PATH) || !(exe = read_file_w(exe_path, &nexe))) {
        lstrcpynA(s_id, id, sizeof s_id);
        refuse("Total Annihilation's exe could not be read");
        return;
    }
    {
        const wchar_t* base = wcsrchr(exe_path, L'\\');
        int sites = tagpu_modimg_from_exe(&s_target, exe, nexe) && is_the_game(&s_target);
        if (!sites && lstrcmpiW(base ? base + 1 : exe_path, L"TotalA.exe")) {
            tagpu_log("modload: this exe has no engine sites to run a mod on, so mod= is left alone");
            free(exe);
            tagpu_modimg_free(&s_target);
            return;
        }
    }
    lstrcpynA(s_id, id, sizeof s_id);
    for (i = 0; id[i]; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9') || id[i] == '_' || id[i] == '-')) {
            refuse("that is not the name of a mod");
            goto done;
        }
    if (!load_pack()) { refuse("this build of Impure has no usable mod pack"); goto done; }
    if (!(mod = tagpu_modpak_mod(&s_pak, id))) { refuse("Impure does not know that mod"); goto done; }
    if (!tagpu_modpak_file_key(exe, nexe, 1, key)) { refuse("Total Annihilation's exe could not be read"); goto done; }
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
    if (!exists(path)) { not_installed(path); goto done; }
    snprintf(path, sizeof path, "mods\\%s\\%s.originals.hpi", id, id);
    if (!exists(path)) { not_installed(path); goto done; }
    /* The mounter's duplicate test resolves a name into 0x100 bytes (0x4BE0D1) and the handle keeps its path in 0x104
       (0x4BDDC1), read back by the prune's open at 0x4BE1B5. This, the longest name it gets, stays inside the smaller. */
    if (!GetFullPathNameA(path, sizeof full, full, NULL) || strlen(full) >= VA_MOUNT_PATH_MAX) {
        refuse("the game's folder is too deep for the mod's files: move it to a shorter path");
        goto done;
    }

    s_xor = &s_pak.xors[mod->xor_];
    s_loader = mod->loader;
    if ((known && !tagpu_modimg_xor(&s_target, &s_pak.xors[known->xor_])) ||
        !tagpu_modimg_xor(&s_target, s_xor)) {
        refuse("this exe's sections do not fit the mod's patch");
        goto done;
    }
    tagpu_modimg_hash(&s_target, s_pak.mask, s_pak.nmask, hash);
    if (memcmp(hash, mod->hash, 32)) { refuse("the patched exe does not have the hash the mod names"); goto done; }

    if (s_loader) {
        tagpu_modimg_xor(&s_target, s_xor);        /* back to stock, for this one write */
        changed = put_image(&s_target, s_pak.mask, s_pak.nmask);
        tagpu_modimg_xor(&s_target, s_xor);
    } else {
        changed = put_image(&s_target, s_pak.mask, s_pak.nmask);
    }
    if (changed < 0) { refuse("the exe's image could not be written (%lu)", GetLastError()); goto done; }
    /* the file everything else compares with: this image as if the file held it, so a table's value at a limit site is the
       mod's own there exactly as in its drop-in folder */
    if (s_loader) tagpu_modimg_xor(&s_target, s_xor);
    for (i = 0; i < s_target.nsec; i++)
        memcpy(exe + s_target.sec[i].raw, s_target.sec[i].bytes, s_target.sec[i].len);
    if (s_loader) tagpu_modimg_xor(&s_target, s_xor);
    s_reference = exe;
    s_nreference = nexe;
    exe = NULL;
    /* s_target is the mod's whole image at this point, a loader mod's list included */
    for (i = 0; i < (int)(sizeof FOLDER_NAMES / sizeof *FOLDER_NAMES); i++) {
        const uint8_t* name = image_at(&s_target, FOLDER_NAMES[i], FOLDER_NAME_MAX);
        if (name && memchr(name, 0, FOLDER_NAME_MAX)) lstrcpynA(s_folder[i], (const char*)name, FOLDER_NAME_MAX);
    }
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

static void box_and_exit(void);

static void hex32(const uint8_t* h, char* out)
{
    int i;
    for (i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

/* mods\archives.cache: name, size, date, SHA-256 of each archive seen. Hashing them all takes seconds, and a
   file changed since has another size or date. A row that does not parse is only hashed again. */
static void load_archive_cache(void)
{
    static char text[ARCHIVES_MAX * 160];
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

/* Whether this file is an original archive of the game, by its bytes: mods reuse and replace the names. -1 when it could not
   be read, which says nothing of the file and leaves no row. */
static int is_original_archive(const char* name)
{
    static uint8_t buf[1u << 20];
    WIN32_FILE_ATTRIBUTE_DATA st;
    unsigned long long size, date;
    ARCHIVE* a = NULL;
    ARCHIVE uncached;
    int keep = 1;
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
        /* a table that is full or a name too long for a row costs a hash on every start, never a wrong answer */
        keep = s_narc < ARCHIVES_MAX && strlen(name) < sizeof a->name;
        a = keep ? &s_arc[s_narc++] : &uncached;
        if (keep) lstrcpynA(a->name, name, sizeof a->name);
    }
    h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (keep && a == &s_arc[s_narc - 1] && !a->size) s_narc--;
        return -1;
    }
    modsha_init(&sha);
    for (;;) {
        if (!ReadFile(h, buf, sizeof buf, &got, NULL)) {
            CloseHandle(h);
            if (keep && a == &s_arc[s_narc - 1]) s_narc--;
            return -1;
        }
        if (!got) break;
        modsha_add(&sha, buf, got);
    }
    CloseHandle(h);
    modsha_end(&sha, a->sha);
    a->size = size;
    a->date = date;
    a->original = tagpu_modpak_is_original(&s_pak, a->sha);
    if (keep) save_archive_cache();
    return a->original;
}

/* ---- mutators -------------------------------------------------------------------------------------

   THE ENGINE'S DIRECTORY, DISASSEMBLED 2026-11-06 (engine/process.md). OpenHAPIFile 0x4BDD70 reads the archive's header and directory into one
   block and turns every offset into a pointer (0x4BDF85, 0x4BE010): handle +8 the block, block +8 its size in the file's header, block +0x10
   the root. A directory is { int count; ENTRY* first }, an entry 9 bytes { char* name; void* child; byte flag }, bit 0 a directory. Lookup
   0x4BB4E0 and the listing 0x4BC640 walk count entries, and nothing keeps a pointer to an entry across the mount; the prune 0x4BE180 reopens
   only the FILE*. So taking an entry out of its directory, on the game thread between a mount's return and the next engine call, is the whole
   of hiding a file. A VALUE IN THE BLOCK IS DATA: each pointer is held inside the block before it is read. */
#define VA_GFX_PP      0x0051FBD0u      /* the graphics globals: 0x4B6220 returns this; the list and its count below */
#define GFX_ARC_LIST   0x618
#define GFX_ARC_COUNT  0x61C
#define HND_BLOCK      0x08
#define HND_PATH       0x14
#define BLOCK_SIZE     0x08
#define BLOCK_ROOT     0x10
#define DIR_ENTRY      9

typedef struct { char archive[48]; char path[MODCAT_PATH]; } GONE;
#define GONE_MAX 1024

/* Written in the first mount call, on the game thread in WinMain, before any thread that reads them exists: a wiki load starts from the shell.
   Nothing writes them afterwards. */
static GONE  s_gone[GONE_MAX];
static int   s_ngone;
static char  s_mutators[200];           /* what mounted: "name version, name version" */
static int   s_nmounted;
static MODMUTATOR s_applied[MUTATORS_MAX];

static void note_catalog_id(void* ctx, const char* id)
{
    IDS* ids = (IDS*)ctx;
    if (ids->n < (int)(sizeof ids->id / sizeof ids->id[0]) && strlen(id) < sizeof ids->id[0]) lstrcpynA(ids->id[ids->n++], id, sizeof ids->id[0]);
}

static int within(const char* block, uint32_t size, const char* p, uint32_t n)
{
    return p >= block && (uint32_t)(p - block) <= size && n <= size - (uint32_t)(p - block);
}

/* The file `path` out of the engine's directory of a mounted archive: how many entries went. Two of one name both go, as the lookup would
   otherwise find the first once the later had gone. */
static int forget_in_engine(char* handle, const char* path)
{
    char* block = *(char**)(handle + HND_BLOCK);
    const uint32_t size = *(const uint32_t*)(block + BLOCK_SIZE);
    char* dir = *(char**)(block + BLOCK_ROOT);
    int gone = 0;
    for (;;) {
        const char* end = strchr(path, '\\');
        const size_t len = end ? (size_t)(end - path) : strlen(path);
        char key[TAGPU_HPI_NAMEMAX];
        char* entries, *next = NULL;
        int count, i;
        if (len >= sizeof key) return gone;
        memcpy(key, path, len);
        key[len] = 0;
        if (!within(block, size, dir, 8)) return gone;
        count = *(int*)dir;
        entries = *(char**)(dir + 4);
        if (count < 0 || (uint32_t)count > size / DIR_ENTRY || !within(block, size, entries, (uint32_t)count * DIR_ENTRY)) return gone;
        for (i = count - 1; i >= 0; i--) {
            char* e = entries + (size_t)i * DIR_ENTRY;
            const char* name = *(char**)e;
            const uint8_t is_dir = (uint8_t)e[8] & 1;
            if (!within(block, size, name, 1) || strnlen(name, size - (uint32_t)(name - block)) == size - (uint32_t)(name - block)) continue;
            if (tagpu_hpi_stricmp(key, name, TAGPU_HPI_NAMEMAX)) continue;
            if (end) {
                if (is_dir && !next) next = *(char**)(e + 4);
            } else if (!is_dir) {
                memmove(e, e + DIR_ENTRY, (size_t)(count - 1 - i) * DIR_ENTRY);
                *(int*)dir = --count;
                gone++;
            }
        }
        if (!end || !next) return gone;
        dir = next;
        path = end + 1;
    }
}

/* the archive the engine appended last, when it is the one mounted just now: the list is append-only (0x4BE162) */
static char* last_mounted(const char* path)
{
    const char* gfx = *(const char* const*)VA_GFX_PP;
    char* const* list;
    int n;
    char* h;
    size_t a, b;
    const char* have;
    if (!gfx || !(list = *(char* const* const*)(gfx + GFX_ARC_LIST)) || (n = *(const int*)(gfx + GFX_ARC_COUNT)) <= 0) return NULL;
    h = list[n - 1];
    have = h + HND_PATH;
    a = strlen(have);
    b = strlen(path);
    return a >= b && !lstrcmpiA(have + a - b, path) ? h : NULL;
}

static void note_gone(const char* archive, const char* path)
{
    if (s_ngone >= GONE_MAX) return;
    lstrcpynA(s_gone[s_ngone].archive, archive, sizeof s_gone[0].archive);
    lstrcpynA(s_gone[s_ngone].path, path, sizeof s_gone[0].path);
    s_ngone++;
}

static int file_off(void* base, const char* path)
{
    return tagpu_profiles_file_of((const char*)base, path) == 0;
}

/* a file.<path> key no mutator of this base now holds: ignored, and said */
static void note_file_key(void* ctx, const char* path)
{
    const char* base = (const char*)ctx;
    int i, k;
    size_t n = 0;
    const char* cat = tagpu_modinst_catalog(&n);
    IDS ids;
    ids.n = 0;
    if (cat) tagpu_modcat_each_id(cat, n, note_catalog_id, &ids);
    for (i = 0; i < ids.n; i++) {
        MODMUTATOR m;
        int hit = 0;
        if (!tagpu_modcat_mutator(cat, n, ids.id[i], &m)) continue;
        if (!strcmp(m.base, base)) {
            for (k = 0; k < m.nfiles && !hit; k++) hit = tagpu_modcat_same_path(m.files[k].path, path);
            for (k = 0; k < m.nhides && !hit; k++) hit = tagpu_modcat_same_path(m.hides[k], path);
        }
        tagpu_modcat_mutator_free(&m);
        if (hit) return;
    }
    tagpu_logf("modload: file.%s names no file of a mutator of %s; ignored", path, base);
}

/* The archives the mount would take for this base, for the fit: the same rule as mount_filtered's, the originals alone under a mod. */
static int mount_names(const char* base, char (*names)[MODFIT_NAME], int max)
{
    int n = tagpu_modfit_folder(names, max), i, kept = 0;
    if (!strcmp(base, FOLDER_BASE)) return n;
    for (i = 0; i < n; i++)
        if (is_original_archive(names[i]) > 0) {
            if (kept != i) memcpy(names[kept], names[i], MODFIT_NAME);
            kept++;
        }
    return kept;
}

/* Ahead of everything the base mounts, in the catalog's order. A switched-on mutator that does not fit stays off for this start: the
   file-by-file check runs again here because a base can have been updated at this very start (tagpu_modinst_sweep), and the screen only says what
   this will do. */
static void mount_mutators(const char* base)
{
    static char names[MODFIT_ARCS][MODFIT_NAME];
    size_t ncat = 0;
    const char* cat = tagpu_modinst_catalog(&ncat);
    IDS ids;
    MODFIT* fit = NULL;
    int i, k, nnames = 0;
    ids.n = 0;
    if (!cat) return;
    tagpu_modcat_each_id(cat, ncat, note_catalog_id, &ids);
    for (i = 0; i < ids.n; i++) {
        MODMUTATOR m;
        MODABOUT about;
        char path[MAX_PATH], archive[48], version[32], stale[8][MODCAT_PATH];
        const char* why = NULL;
        int total;
        if (!tagpu_modcat_mutator(cat, ncat, ids.id[i], &m)) continue;
        if (strcmp(m.base, base) || tagpu_profiles_mutator_of(base, m.id) != 1) { tagpu_modcat_mutator_free(&m); continue; }
        if (!tagpu_modinst_installed(m.id, version, sizeof version)) {
            tagpu_logf("modload: mutator %s is switched on and not installed", m.id);
            tagpu_modcat_mutator_free(&m);
            continue;
        }
        if (!fit) {
            nnames = mount_names(base, names, MODFIT_ARCS);
            fit = tagpu_modfit_open(base, (const char (*)[MODFIT_NAME])names, nnames);
        }
        total = fit ? tagpu_modcat_stale(&m, tagpu_modfit_hash, fit, stale, 8) : 1;
        if (!fit) lstrcpynA(stale[0], "?", sizeof stale[0]);
        why = tagpu_modcat_stale_live(stale, total, 8, file_off, (void*)base);
        if (why) {
            tagpu_logf("modload: not mounted mutator %s: %s differs from the file it was built on", m.id, why);
            tagpu_modcat_mutator_free(&m);
            continue;
        }
        for (k = 0; k < s_nmounted && !why; k++) {
            if ((why = tagpu_modcat_clash(&m, &s_applied[k], file_off, (void*)base)) != NULL)
                tagpu_logf("modload: not mounted mutator %s: %s is %s's too", m.id, why, s_applied[k].id);
        }
        if (why) { tagpu_modcat_mutator_free(&m); continue; }

        snprintf(path, sizeof path, "mods\%s\%s.hpi", m.id, m.id);
        snprintf(archive, sizeof archive, "%s.hpi", m.id);
        if (s_nmounted >= MUTATORS_MAX || !s_mountReal(path, 1)) {
            tagpu_logf("modload: mutator %s does not open", m.id);
            tagpu_modcat_mutator_free(&m);
            continue;
        }
        {
            char* h = last_mounted(path);
            for (k = 0; k < m.nfiles; k++)
                if (file_off((void*)base, m.files[k].path)) {
                    tagpu_logf("modload: %s: %s is switched off, %d entries", m.id, m.files[k].path, h ? forget_in_engine(h, m.files[k].path) : 0);
                    note_gone(archive, m.files[k].path);
                }
        }
        if (tagpu_modcat_about(cat, ncat, m.id, &about)) {
            size_t at = strlen(s_mutators);
            snprintf(s_mutators + at, sizeof s_mutators - at, "%s%s %s", at ? ", " : "", about.name, version);
        }
        tagpu_logf("modload: mounted mutator %s %s: %d files, %d hides", m.id, version, m.nfiles, m.nhides);
        s_applied[s_nmounted++] = m;    /* the table keeps its arrays */
    }
    if (fit) tagpu_modfit_close(fit);
    tagpu_profiles_each_file(base, note_file_key, (void*)base);
}

/* What the mounted mutators hide from the base's own archives, right after each has mounted; a later source answers the path. */
static void apply_hides(const char* path, const char* archive)
{
    char* h = last_mounted(path);
    int i, k;
    if (!h) return;
    for (i = 0; i < s_nmounted; i++)
        for (k = 0; k < s_applied[i].nhides; k++) {
            const char* p = s_applied[i].hides[k];
            if (file_off(s_id, p)) continue;
            tagpu_logf("modload: %s hides %s from %s: %d entries", s_applied[i].id, p, archive, forget_in_engine(h, p));
            note_gone(archive, p);
        }
}

const char* tagpu_modload_mutators(void)
{
    return s_mutators;
}

void tagpu_modload_each_gone(void (*fn)(void* ctx, const char* archive, const char* path), void* ctx)
{
    int i;
    for (i = 0; i < s_ngone; i++) fn(ctx, s_gone[i].archive, s_gone[i].path);
}

static int __stdcall mount_filtered(const char* name, int flag)
{
    const char* base = name, *p;
    if (!s_ours) {
        char path[MAX_PATH], arch[48];
        s_ours = 1;
        mount_mutators(s_id[0] ? s_id : FOLDER_BASE);
        if (!s_id[0]) return s_mountReal(name, flag);
        /* The list is append-only (0x4BE162), so this call, the first, is what puts them ahead of everything. Flag 1 is what
           the mods' own first pattern passes. A package that does not open would leave the mod's exe on retail's data. */
        snprintf(path, sizeof path, "mods\\%s\\%s.hpi", s_id, s_id);
        if (!s_mountReal(path, 1)) {
            refuse("%s does not open: install the mod again", path);
            box_and_exit();
        }
        snprintf(arch, sizeof arch, "%s.hpi", s_id);
        apply_hides(path, arch);
        snprintf(path, sizeof path, "mods\\%s\\%s.originals.hpi", s_id, s_id);
        if (!s_mountReal(path, 1)) {
            refuse("%s does not open: install the mod again", path);
            box_and_exit();
        }
        snprintf(arch, sizeof arch, "%s.originals.hpi", s_id);
        apply_hides(path, arch);
        tagpu_logf("modload: mounted mods\\%s\\%s.hpi and its originals", s_id, s_id);
        if (!exists(ESSENTIAL_ARCHIVE)) {
            refuse("this folder has no %s, so this copy of the game cannot run mods", ESSENTIAL_ARCHIVE);
            box_and_exit();
        }
    }
    if (!s_id[0]) return s_mountReal(name, flag);
    for (p = name; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') base = p + 1;
    if (lstrcmpiA(base, IMPURE_UFO)) {
        static char seen[ARCHIVES_MAX][64];
        static int nseen;
        const char* why = NULL;
        /* the player's hide.<archive> over the mod's list; the archive every mod needs stays whatever the file says */
        const int key = lstrcmpiA(base, ESSENTIAL_ARCHIVE) ? tagpu_profiles_hide_of(s_id, base) : 0;
        if (key > 0) why = "your profile hides it";
        else if (key < 0 && tagpu_modcat_hidden((const char*)s_catalog, s_ncatalog, s_id, base)) why = "the mod hides it";
        else {
            int original = is_original_archive(name);
            if (original <= 0) why = original < 0 ? "it could not be read" : "it is no original archive of the game";
            /* Every mod needs this one. Without it the game fails later, for a reason no box names: a retail edition whose
               archives have other hashes than the ones the pack was built from. */
            if (why && !lstrcmpiA(base, ESSENTIAL_ARCHIVE)) {
                refuse(original < 0 ? "this folder's %s could not be read"
                                    : "this folder's %s is not one Impure knows, so this copy of the game cannot run mods", base);
                box_and_exit();
            }
        }
        if (why) {
            int i;
            for (i = 0; i < nseen && lstrcmpiA(seen[i], base); i++) {}
            if (i == nseen && nseen < ARCHIVES_MAX) {
                lstrcpynA(seen[nseen++], base, sizeof seen[0]);
                tagpu_logf("modload: not mounted %s: %s", base, why);
            }
            return 0;   /* as for one already mounted: *.HPI's ten-a-call count does not move (0x41D5D9) */
        }
    }
    return s_mountReal(name, flag);
}

/* ---- what the Mods screen asks ---------------------------------------------------------------------- */

const char* tagpu_modload_essential_archive(void)
{
    return ESSENTIAL_ARCHIVE;
}

int tagpu_modload_exe_known(void)
{
    static int s_tried;
    wchar_t path[MAX_PATH];
    uint8_t* exe;
    uint8_t key[32];
    size_t n = 0;
    int known = 0;
    if (!s_id[0] && !s_tried) {
        s_tried = 1;
        if (!load_pack()) tagpu_log("modload: this build has no usable mod pack");
    }
    if (!s_blob) return 0;
    if (!GetModuleFileNameW(NULL, path, MAX_PATH) || !(exe = read_file_w(path, &n))) return 0;
    if (tagpu_modpak_file_key(exe, n, 1, key))
        known = !memcmp(key, s_pak.stock_key, 32) || tagpu_modpak_known(&s_pak, 0, key);
    free(exe);
    return known;
}

int tagpu_modload_archive_is_original(const char* path)
{
    static unsigned char buf[1 << 16];
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    MODSHA sha;
    uint8_t out[32];
    DWORD got;
    if (h == INVALID_HANDLE_VALUE) return -1;
    modsha_init(&sha);
    for (;;) {
        if (!ReadFile(h, buf, sizeof buf, &got, NULL)) { CloseHandle(h); return -1; }
        if (!got) break;
        modsha_add(&sha, buf, got);
    }
    CloseHandle(h);
    modsha_end(&sha, out);
    return tagpu_modpak_is_original(&s_pak, out);
}

/* ---- the loose step ---------------------------------------------------------------------------- */

/* The folders the engine reads through its file open, the list modpack.py's GAME_DIRS packs from. The rest is
   no game data: the saves, what a player keeps beside the game. */
int tagpu_modload_loose_closed(const char* path)
{
    static const char* const dirs[] = {
        "anims", "bitmaps", "camera", "camps", "cdplayer", "download", "features", "fonts", "gamedata", "guis", "maps",
        "objects3d", "palettes", "scripts", "sections", "sounds", "textures", "unitpics", "units", "ai", "weapons",
        "unitscript", "unitsb", "cursors", "unittextures"
    };
    char first[32];
    unsigned i, n = 0;
    char* lang;
    if (!s_id[0] || !path) return 0;
    if (path[0] == '.' && (path[1] == '\\' || path[1] == '/')) path += 2;
    while (path[n] && path[n] != '\\' && path[n] != '/') {
        if (n >= sizeof first - 1) return 0;
        first[n] = path[n];
        n++;
    }
    if (!path[n]) return 0;     /* a bare name: no folder of game data */
    first[n] = 0;
    /* the path builder tries "<dir>-<language>\\<name>" first (0x4290F0): the same folder */
    if ((lang = strchr(first, '-'))) *lang = 0;
    for (i = 0; i < sizeof dirs / sizeof *dirs; i++) if (!lstrcmpiA(first, dirs[i])) return 1;
    for (i = 0; i < sizeof s_folder / sizeof *s_folder; i++) if (s_folder[i][0] && !lstrcmpiA(first, s_folder[i])) return 1;
    return 0;
}

/* The log names each once, up to a bound. These filters run on more than one thread and every InitTAHPIAry lists again; a race
   costs a repeated line. */
#define NOTED_MAX 32
static char s_noted[NOTED_MAX][64];
static int s_nnoted;

static int note_once(const char* path)
{
    int i;
    for (i = 0; i < s_nnoted; i++) if (!lstrcmpiA(s_noted[i], path)) return 0;
    if (s_nnoted >= NOTED_MAX) return 0;
    lstrcpynA(s_noted[s_nnoted++], path, sizeof s_noted[0]);
    return 1;
}

static void note_closed(const char* path)
{
    if (exists(path) && note_once(path)) {
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

/* Whether a listing hid anything, so the log can name it: the listing's own cost once over, on a folder usually absent. */
static void note_listing(const char* spec)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    FindClose(h);
    if (note_once(spec)) tagpu_logf("modload: the loose files %s are not listed under a mod", spec);
}

static long __cdecl find_filtered(const char* spec, void* data)
{
    if (tagpu_modload_loose_closed(spec)) {
        note_listing(spec);
        return -1;
    }
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

/* the store first: a player who kills the box still gets the folder's own game next time */
static void box_and_exit(void)
{
    static char text[sizeof s_refusal + 200];
    if (!s_keep_choice) tagpu_settings_set_mod("");
    tagpu_settings_flush();
    snprintf(text, sizeof text, "The mod \"%s\" cannot start:\n\n%s.\n\n%s", s_id, s_refusal,
             s_keep_choice ? "It is still your choice for the next start." : "The next start runs without it.");
    tagpu_refuse(text);
}

/* the exe's entry point, reached on its one thread before any of its code */
void tagpu_modload_entry_stage(void)
{
    const MODRANGE jump = { (uint32_t)(uintptr_t)tagpu_modload_entry_target, 5 };
    int changed;
    s_entered = 1;
    if (s_refusal[0]) box_and_exit();
    changed = put_image(&s_snapshot, &jump, 1);     /* the count is of the others' bytes */
    if (changed < 0 || put_image(&s_snapshot, NULL, 0) < 0) {
        refuse("the exe's image could not be written (%lu)", GetLastError());
        box_and_exit();
    }
    tagpu_logf("modload: at the entry point: %d B other DLLs wrote were put back", changed);
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
    if (!s_id[0]) {
        /* a launch with no mod takes the detour only for mutators the player switched on; a patched exe keeps its own game */
        if (s_folderMutators && tagpu_modload_is_game() && !install_mount())
            tagpu_log("modload: this exe's archive code is not what Impure expects, so no mutator of the folder's game mounts");
        return;
    }
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
    if (!tagpu_detour_write(entry, jump, 5)) refuse("the exe's entry point could not be written (%lu)", GetLastError());
}

void tagpu_modload_check(void)
{
    if (!s_id[0] || s_entered) return;
    refuse("Impure's check at the exe's entry point never ran: another DLL in this folder has most likely taken the entry point");
    box_and_exit();
}
