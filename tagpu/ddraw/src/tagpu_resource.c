#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "tagpu_resource.h"
#include "tagpu_profiles.h"
#include "tagpu_engine.h"
#include "tagpu_limits.h"
#include "tagpu_detour.h"
#include "tagpu_replay.h"
#include "tagpu_puppet.h"
#include "tagpu_log.h"
#include "tagpu_vpwide.h"
#include "tagpu_buildmenu.h"
#include "tagpu_packet_pub.h"
#include "winapi_hooks.h"

/* DISASSEMBLED: 0x419670 uses these order roots and reads no build list
   (0x41975E..0x4197A9). Quick Build's candidates are each constructor's
   build menu (tagpu_buildmenu.h). The input and placement contracts are
   recorded in resource-snapping.md and exe-reverse-engineering.md. All
   engine access belongs to the input/publish thread, between simulation
   iterations. No pointer is retained across engine calls that can tear a
   level down. */
#define D_STRIDE 0x249u
#define D_BASE 0x1439B
#define U_FLAGS 0x110
#define U_ORDERS 0x5C
#define D_FLAGS 0x241
#define M_POINT 0x2CAA
#define M_MODE 0x2CC3
#define M_TYPE 0x2CC4
#define M_REGION 0x2CC6
#define PATCH_R 64
#define PATCH_SIDE (PATCH_R * 2 + 1)
#define PATCH_CAP (PATCH_SIDE * PATCH_SIDE)

typedef int (__stdcall* GridTest)(const void*, unsigned, unsigned, const void*);
typedef unsigned (__stdcall* SiteHeight)(void);
typedef unsigned (__stdcall* BlockedHeight)(const void*, unsigned);
typedef const unsigned char* (__stdcall* Resolve)(int*, unsigned, void*, void*, const void*);
typedef void (__stdcall* Order)(int, int, void*, void*, const void*, int, int);
typedef void (__stdcall* RemoveOrder)(void*, const void*);
typedef unsigned char* (__attribute__((thiscall))* Action)(void*, const char*);
/* The page loader's calls (0x41ACE0) and the file layer as the TDF loader 0x4C2F60 calls it,
   all stdcall (DISASSEMBLED, the `ret` sizes in exe-reverse-engineering.md beside 0x4C2F60). */
typedef char* (__stdcall* PathFn)(char*, const char*, const char*, const char*);  /* 0x4290F0, ret 0x10 */
typedef int (__stdcall* SizeFn)(const char*);              /* 0x4BBC40, ret 4: the size, 0 missing */
typedef void* (__stdcall* OpenFn)(const char*);            /* 0x4BB5B0, ret 4: handle or NULL */
typedef int (__stdcall* LengthFn)(void*);                  /* 0x4BBD00, ret 4 */
typedef int (__stdcall* SeekFn)(void*, int);               /* 0x4BB710, ret 8 */
typedef int (__stdcall* ReadFn)(void*, void*, int);        /* 0x4BB7C0, ret 0xC: bytes read */
typedef int (__stdcall* CloseFn)(void*);                   /* 0x4BB5D0, ret 4 */
typedef unsigned (__stdcall* TypeOfFn)(const char*);       /* 0x488B10, ret 4: the type in ax */

typedef struct {
    char *ta, *defs, *units, *grid;
    unsigned ndefs, nslots, first, last;
    int w, h, player, threshold, shift;
} World;
typedef struct {
    unsigned type, target;
    int kind, x, z, height, queued, anchor_x, anchor_z;
    float strength;
} Choice;
typedef struct {
    const char *build, *reclaim;
} Cancel;

/* UI state, game-thread owned: the engine simulation never reads it. */
static int s_armed, s_raw[3], s_raw_valid;
static unsigned char s_patch_seen[PATCH_CAP];
static unsigned short s_patch_queue[PATCH_CAP];

/* A constructor type's build menu: the types its pages arm, ascending. GAME THREAD ONLY.
   THE TABLE'S KEY is the def table it was read against: its base main+0x1439B and its count
   main+0x1438F, as values that are compared and never dereferenced, and the level generation
   (tagpu_packet_pub_level_gen). DISASSEMBLED: the level teardown 0x42DB90 frees the def array
   (0x42DCCB) and zeroes its pointer (0x42DCE6), and the menu-time loader 0x42A8D0 allocates a
   new one before the next level (0x42AA8A..0x42AA98). So every level has a new allocation, which
   the base or the count shows with no teardown observer installed, unless the heap hands back
   the freed block's address for the same count; the generation, which moves only while a
   teardown observer is installed, covers that case. With no observer and that reuse, the table
   outlives its level; its
   content is then still the new level's [INFERRED: the archives are mounted once a process, so
   every level reads the same defs, download files and GUI pages]. An entry is built on its
   first use for the watched player's side and rebuilt when that side changes.
   THE TABLE'S LIFETIME: it holds type numbers only, and every number read out of it is bounded
   against the live def count before it addresses a def, so even a table kept past its level
   costs a wrong candidate, never a read outside the defs. */
typedef struct { unsigned short* types; unsigned short n; unsigned char side, built; } Menu;
static Menu* s_menu;
static unsigned s_menuN, s_menuGen;
static uintptr_t s_menuDefs;

static int world(World* w)
{
    char *state, *end, *player;
    uintptr_t first, last;
    uintptr_t distance;
    memset(w, 0, sizeof *w);
    w->ta = *(char**)TA_MAIN_PP;
    if (!w->ta) return 0;
    w->player = *(unsigned char*)(w->ta + OFF_WATCHED);
    if (w->player >= 10 || tagpu_puppet_seat(w->player)) return 0;
    w->w = *(int*)(w->ta + OFF_MAP_W16); w->h = *(int*)(w->ta + OFF_MAP_H16);
    /* LoadMap's row product is the allocation; bound it before any index or
       byte product. The input path cannot run while the loader owns it. */
    if (w->w < 2 || w->h < 2 || (w->w & 1) || (w->h & 1) || w->w > 0x7FFF || w->h > 0x7FFF ||
        (unsigned)w->w > 0x7FFFFFFFu / FT_STRIDE / (unsigned)w->h) return 0;
    w->ndefs = *(unsigned*)(w->ta + OFF_UDEFCOUNT);
    if (!w->ndefs || w->ndefs > TAGPU_LIM_TYPES) return 0;
    w->defs = *(char**)(w->ta + D_BASE);
    w->units = *(char**)(w->ta + OFF_UNIT_BEGIN);
    end = *(char**)(w->ta + OFF_UNIT_END);
    w->grid = *(char**)(w->ta + OFF_FEATMAP);
    state = *(char**)(w->ta + 0x391E9);
    if (!w->defs || !w->units || !w->grid || !state || (uintptr_t)end < (uintptr_t)w->units) return 0;
    distance = (uintptr_t)end - (uintptr_t)w->units;
    if (distance % UNIT_STRIDE || distance / UNIT_STRIDE >= TAGPU_PK_DESIGN_SLOTS) return 0;
    w->nslots = *(unsigned short*)(w->ta + 0x14351);
    if (w->nslots != (unsigned)(distance / UNIT_STRIDE) + 1) return 0;
    player = w->ta + 0x1B63 + w->player * 0x14B;
    first = (uintptr_t)*(char**)(player + 0x67); last = (uintptr_t)*(char**)(player + 0x6B);
    if (first < (uintptr_t)w->units || last < first || last > (uintptr_t)end ||
        (first - (uintptr_t)w->units) % UNIT_STRIDE ||
        (last - (uintptr_t)w->units) % UNIT_STRIDE) return 0;
    w->first = (unsigned)((first - (uintptr_t)w->units) / UNIT_STRIDE);
    w->last = (unsigned)((last - (uintptr_t)w->units) / UNIT_STRIDE);
    w->threshold = *(unsigned*)(state + 0xD30) > 255u ? 255 : *(unsigned*)(state + 0xD30);
    w->shift = (fake_GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    return 1;
}

static char* unit(const World* w, unsigned slot)
{
    char* u;
    unsigned flags;
    if (!slot || slot >= w->nslots) return NULL;
    u = w->units + slot * UNIT_STRIDE;
    flags = *(unsigned*)(u + U_FLAGS);
    return (flags & 0x10000000u) && !(flags & 0x4000u) ? u : NULL;
}

static unsigned def_id(const World* w, const char* u)
{
    uintptr_t d = (uintptr_t)*(char* const*)(u + U_TYPE), base = (uintptr_t)w->defs;
    uintptr_t offset;
    if (d < base) return 0;
    offset = d - base;
    if (offset % D_STRIDE || offset / D_STRIDE >= w->ndefs) return 0;
    return (unsigned)(offset / D_STRIDE);
}

static int footprint(const char* d, int* x, int* z)
{
    *x = *(short*)(d + 0x14A); *z = *(short*)(d + 0x14C);
    return *x > 0 && *z > 0 && *x <= 64 && *z <= 64;
}

static int kind(const char* d)
{
    const unsigned char* yard;
    float metal = *(float*)(d + 0x1CE);
    int fx, fz;
    if (d[0x22F] || !footprint(d, &fx, &fz) ||
        !*(const unsigned char* const*)(d + 0x14E)) return 0;
    if (isfinite(metal) && metal > 0) return 1;
    /* The loader allocates exactly FootX*FootZ yard bytes (0x42CF30).
       Unlike TADR's 64-byte heuristic, inspect the complete bounded yard. */
    yard = *(const unsigned char* const*)(d + 0x14E);
    return yard && memchr(yard, 0x8F, (size_t)fx * fz) ? 2 : 0;
}

static float strength(const char* d, int k)
{
    float v = *(float*)(d + (k == 1 ? 0x1CE : 0x1C2));
    return isfinite(v) && v > 0 ? v : 0;
}

static int selected(const World* w, const char* u)
{
    return *(unsigned char*)(u + U_OWNER) == w->player && (*(unsigned*)(u + U_FLAGS) & 0x10u);
}

/* ---- the build menu, Quick Build's candidates --------------------------------------------

   THE ORDERING THAT MAKES EVERY CALL BELOW SAFE: build_menu runs only inside quick_choice's
   callers -- the left and right command gates 0x498F70 / 0x499100 and the packet fill -- on the
   GAME thread, in play, between two simulation steps and never inside an engine file call. That
   is the thread and the phase in which the engine itself calls the page loader 0x41ACE0, its
   GUI_Load 0x4AA8F0 and the TDF loader 0x4C2F60, which make these same calls; a puppet seat
   never gets here (world()). The def array, the download records and the side table it reads
   are written by the loader before the first in-play step and freed by the teardown 0x491B60,
   which runs on this thread. The rule itself is tagpu_buildmenu.c; this is its engine half. */

#define M_DL_COUNT 0x391C7          /* int: the download records, 0x42DCF0 */
#define M_DL_BLOCK 0x391CB          /* the records, 0xBD bytes each (0x41AEEF) */
#define DL_STRIDE 0xBDu
#define DL_ENTRY 0x25u              /* five entries a record from +4: {u16 builder, MENU, BUTTON, name[32]} */
#define M_SIDES 0x37F39             /* u32: the sides this game loaded */
#define M_SIDE_PREFIX 0x37F5B       /* a side's nameprefix, read with size 4 (0x431B26..0x431B3B) */
#define SIDE_STRIDE 0x232u
#define SIDE_ROOM 5u                /* the engine's five side records (0x431A20's free loop) */
#define M_GUI_DIR 0xECF             /* GUI_Load's prefix gi+0x9B6, gi = main+0x519, set once at 0x4914CE */

static unsigned bm_type_of(void* ctx, const char* name)
{
    const World* w = (const World*)ctx;
    /* 0x488B10 answers the type word in ax and leaves the rest of eax undefined (0x488BC1) */
    unsigned t = ((TypeOfFn)0x00488B10u)(name) & 0xFFFFu;
    return t < w->ndefs ? t : 0;
}

static int bm_armable(void* ctx, unsigned type)
{
    const World* w = (const World*)ctx;
    return type && type < w->ndefs && !w->defs[type * D_STRIDE + 0x22F];
}

/* The watched player's side, as 0x41ACE0 takes it for the DL page (0x41AD61..0x41AD66):
   PlayerInfo at the player record +0x27, its byte +0x95. PlayerInfo objects live for the
   process. BOUND: below the sides loaded and the five records; 0xFF when out of bounds. */
static unsigned char watched_side(const World* w)
{
    const unsigned char* info =
        *(const unsigned char* const*)(w->ta + OFF_PLAYERS + w->player * 0x14B + PL_INFO);
    unsigned sides = *(const unsigned*)(w->ta + M_SIDES), side;
    if (!info) return 0xFF;
    side = info[0x95];
    return side < sides && side < SIDE_ROOM ? (unsigned char)side : 0xFF;
}

/* This builder's download entries in the engine's order (0x41AE0F..0x41AEFF). BOUND: records
   below [main+0x391C7], which lie inside the block in stock's layout and in fix_download_records'
   (tagpu_patches.c, its THE INVARIANT); entries below min(count, 5). Where the download fix is
   absent (the stock-limits build), a file of six or more sections gives a record a count past
   five, and the engine reads on past its fifth entry into the next record: this read stops at
   the fifth, inside the record. An entry's name is the module's to bound (its 32 bytes). */
static TAGPU_BM_ENTRY* dl_entries(const World* w, unsigned short builder, unsigned* n)
{
    const unsigned char* block = *(const unsigned char* const*)(w->ta + M_DL_BLOCK);
    int records = *(const int*)(w->ta + M_DL_COUNT), r;
    TAGPU_BM_ENTRY* out = NULL;
    unsigned room = 0;
    *n = 0;
    if (!block) return NULL;
    for (r = 0; r < records; r++) {
        const unsigned char* rec = block + (size_t)r * DL_STRIDE;
        int count = *(const int*)rec, k;
        if (count > 5) count = 5;
        for (k = 0; k < count; k++) {
            const unsigned char* e = rec + 4 + (unsigned)k * DL_ENTRY;
            if (*(const unsigned short*)e != builder) continue;
            if (*n == room) {
                TAGPU_BM_ENTRY* bigger;
                room = room ? room * 2 : 8;
                if (!(bigger = (TAGPU_BM_ENTRY*)realloc(out, room * sizeof *out))) { free(out); *n = 0; return NULL; }
                out = bigger;
            }
            out[*n].menu = e[2];
            out[*n].button = e[3];
            memcpy(out[*n].name, e + 4, TAGPU_BM_ENTRY_NAME);
            (*n)++;
        }
    }
    return out;
}

/* The file GUI_Load reads, whole, through the engine's own file layer as 0x4C2F60 calls it.
   BOUND: 0 < size <= 1 MiB into our own buffer, and the read must return exactly the size.
   0x4BBE50 is not used: its allocation 0x4D83B0 is not NULL-checked. */
static unsigned char* page_bytes(const char* path, size_t* size, const char** why)
{
    void* h = ((OpenFn)0x004BB5B0u)(path);
    unsigned char* buf;
    int n;
    *size = 0;
    if (!h) { *why = "the file does not open"; return NULL; }
    n = ((LengthFn)0x004BBD00u)(h);
    if (n <= 0 || (unsigned)n > TAGPU_BM_FILE_CAP) {
        ((CloseFn)0x004BB5D0u)(h);
        *why = n <= 0 ? "an empty file" : "a file past 1 MiB";
        return NULL;
    }
    if (!(buf = (unsigned char*)malloc((size_t)n))) {
        ((CloseFn)0x004BB5D0u)(h);
        *why = "out of memory";
        return NULL;
    }
    ((SeekFn)0x004BB710u)(h, 0);
    if (((ReadFn)0x004BB7C0u)(h, buf, n) != n) {
        ((CloseFn)0x004BB5D0u)(h);
        free(buf);
        *why = "a short read";
        return NULL;
    }
    ((CloseFn)0x004BB5D0u)(h);
    *size = (size_t)n;
    return buf;
}

/* One page's file name as 0x41ACE0 chooses it: its own "<NAME><page>.GUI" when the probe
   0x4290F0 + 0x4BBC40 answers a size, else the watched side's "<prefix>DL". BOUND: the probe's
   output is 1024 bytes and 0x4290F0 writes with no bound of its own; the folder 0x502820 and the
   language 0x51FB50 (0x49F580) under 256 bytes each and a name of at most 36 keep its longest
   form, "<folder>-<language>\<name>.GUI", inside it (map_file_opens' argument). */
static int page_file(const World* w, const char* d, unsigned page, unsigned char side,
                     char* name, size_t cap, const char** why)
{
    char probe[1024];
    const char* prefix;
    if (!tagpu_buildmenu_page_name(name, cap, d + 0x20, page)) { *why = "no room for the name"; return 0; }
    if (strnlen((const char*)0x00502820u, 256) >= 256 || strnlen((const char*)0x0051FB50u, 256) >= 256) {
        *why = "the GUI folder or the language is past 256 bytes";
        return 0;
    }
    ((PathFn)0x004290F0u)(probe, (const char*)0x00502820u, name, (const char*)0x00502828u);
    if (((SizeFn)0x004BBC40u)(probe)) return 1;
    if (side == 0xFF) { *why = "no own page and no side for the DL page"; return 0; }
    prefix = w->ta + M_SIDE_PREFIX + side * SIDE_STRIDE;
    if (strnlen(prefix, 4) >= 4) { *why = "the side's prefix is past 3 characters"; return 0; }
    snprintf(name, cap, "%sDL", prefix);
    return 1;
}

static void menu_read(const World* w, unsigned id, unsigned char side, Menu* m)
{
    const char* d = w->defs + id * D_STRIDE;
    unsigned char pages[TAGPU_BM_PAGES], *offered;
    TAGPU_BM_ENGINE eng;
    TAGPU_BM_ENTRY* entries;
    LARGE_INTEGER t0, t1, hz;
    unsigned np, nentries, p, t, read = 0, extractors = 0, geos = 0;
    QueryPerformanceCounter(&t0);
    np = tagpu_buildmenu_pages((*(const unsigned*)(d + D_FLAGS) & 0x80000000u) != 0,
                               *(const unsigned char*)(d + 0x22E), pages);
    entries = dl_entries(w, *(const unsigned short*)(d + 0x21E), &nentries);
    if (!(offered = (unsigned char*)calloc(w->ndefs, 1))) { free(entries); return; }
    eng.type_of = bm_type_of; eng.armable = bm_armable; eng.ctx = (void*)w; eng.ndefs = w->ndefs;
    for (p = 0; p < np; p++) {
        char name[64], path[0x100 + 64], why[160];
        const char* refused = NULL;
        unsigned char* blob;
        size_t size;
        if (!page_file(w, d, pages[p], side, name, sizeof name, &refused))
            ;   /* `refused` says why */
        else if (!tagpu_buildmenu_gui_path(path, sizeof path, w->ta + M_GUI_DIR, name))
            refused = "the GUI path is past its buffer";
        else if ((blob = page_bytes(path, &size, &refused)) != NULL) {
            if (tagpu_buildmenu_page(blob, size, pages[p], entries, nentries, &eng, offered,
                                     why, sizeof why) < 0) refused = why;
            else read++;
            free(blob);
        }
        if (refused) tagpu_logf("resource: menu %.31s page %u: %s", d + 0x20, pages[p], refused);
    }
    free(entries);
    for (t = 1; t < w->ndefs; t++) if (offered[t]) m->n++;
    if (m->n && (m->types = (unsigned short*)malloc(m->n * sizeof *m->types)) != NULL) {
        unsigned k = 0;
        for (t = 1; t < w->ndefs; t++) if (offered[t]) {
            int kd = kind(w->defs + t * D_STRIDE);
            m->types[k++] = (unsigned short)t;
            extractors += kd == 1; geos += kd == 2;
        }
    } else m->n = 0;
    free(offered);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&hz);
    tagpu_logf("resource: menu %.31s side=%d pages=%u read=%u offers=%u extractors=%u geos=%u ms=%.2f",
               d + 0x20, side == 0xFF ? -1 : (int)side, np, read, m->n, extractors, geos,
               (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart);
}

/* The menu of def `id` (a builder) for the watched side, built on its first use against this
   def table (the key above). GAME THREAD ONLY (the ordering above). The pointer is good until
   the next call. */
static const Menu* build_menu(const World* w, unsigned id)
{
    unsigned gen = tagpu_packet_pub_level_gen(), i;
    unsigned char side;
    Menu* m;
    if (!id || id >= w->ndefs) return NULL;
    if (!s_menu || gen != s_menuGen || (uintptr_t)w->defs != s_menuDefs || w->ndefs != s_menuN) {
        for (i = 0; s_menu && i < s_menuN; i++) free(s_menu[i].types);
        free(s_menu);
        s_menuN = 0;
        if (!(s_menu = (Menu*)calloc(w->ndefs, sizeof *s_menu))) return NULL;
        s_menuN = w->ndefs;
        s_menuGen = gen;
        s_menuDefs = (uintptr_t)w->defs;
    }
    side = watched_side(w);
    m = &s_menu[id];
    if (m->built && m->side == side) return m;
    free(m->types);
    memset(m, 0, sizeof *m);
    menu_read(w, id, side, m);
    m->side = side;
    m->built = 1;
    return m;
}

static int menu_offers(const World* w, unsigned id, unsigned type)
{
    const Menu* m = build_menu(w, id);
    unsigned lo = 0, hi;
    if (!m || !type || type >= w->ndefs) return 0;
    hi = m->n;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (m->types[mid] < type) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->n && m->types[lo] == type;
}

static int can_build(const World* w, const char* u, unsigned type)
{
    unsigned id = def_id(w, u);
    if (!id) return 0;
    if (!(*(unsigned*)(w->defs + id * D_STRIDE + D_FLAGS) & 0x40u)) return 0;
    return menu_offers(w, id, type);
}

static int can_assist(const World* w, const char* u)
{
    unsigned id = def_id(w, u);
    const char* d;
    if (!id) return 0;
    d = w->defs + id * D_STRIDE;
    return (*(unsigned*)(d + D_FLAGS) & 0x40u) &&
           (*(unsigned*)(d + 0x245) & 0x200u) && *(unsigned short*)(d + 0x1FE);
}

static int discovered(const World* w, int x, int z)
{
    const unsigned short* mapped;
    unsigned bit;
    if (x < 0 || z < 0 || x >= w->w || z >= w->h) return 0;
    mapped = *(const unsigned short* const*)(w->ta + 0x14273);
    bit = *(unsigned char*)(w->ta + 0x1B63 + 0x14B * w->player + 0x146);
    if (!mapped || bit >= 16) return 0;
    /* LoadMap allocates W*H/4 u16 entries; dimensions are even; the setter
       0x481930 uses the player's map bit, not its seat number. */
    return (mapped[(z / 2) * (w->w / 2) + x / 2] & (1u << bit)) != 0;
}

static int metal(const World* w, int x, int z)
{
    return discovered(w, x, z) ? (unsigned char)w->grid[((size_t)z * w->w + x) * FT_STRIDE + 7] : -1;
}

static int coverage(const World* w, int x, int z, int fx, int fz)
{
    int dx, dz, count = 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++)
        if (metal(w, x + dx, z + dz) > w->threshold) count++;
    return count;
}

/* The footprint inside the map's placeable interior and every cell of it explored: what fits()
   asks before the engine's site test, and all a queued plan's site has to satisfy. */
static int site_known(const World* w, unsigned type, int x, int z)
{
    int fx, fz, dx, dz;
    if (!footprint(w->defs + type * D_STRIDE, &fx, &fz) || x < 1 || z < 1 ||
        x + fx >= w->w || z + fz >= w->h ||
        x * 16 + fx * 8 > 32767 || z * 16 + fz * 8 > 32767) return 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++)
        if (!discovered(w, x + dx, z + dz)) return 0;
    return 1;
}

/* DISASSEMBLED 0x47D820, stdcall (def, x | z << 16), `ret 8`: the altitude the build cursor gives
   a blocked site (0x4198FE; 0x47C780 at 0x4198F5 gives a free one's, from the site test's
   scratch). It reads the def's footprint and yardmap and the map's heights and sea level, writes
   nothing outside its own frame, and answers 0 for a footprint outside [1, w) x [1, h). */
static int blocked_height(const World* w, unsigned type, int x, int z)
{
    return (int)(((BlockedHeight)0x0047D820u)(w->defs + type * D_STRIDE,
                                              (unsigned)x | (unsigned)z << 16) & 255);
}

static int fits(const World* w, unsigned type, int x, int z, unsigned ignore, int* height)
{
    char* d = w->defs + type * D_STRIDE;
    int fx, fz, dx, dz, ok;
    unsigned char copy[D_STRIDE], yard[64 * 64];
    if (!site_known(w, type, x, z) || !footprint(d, &fx, &fz)) return 0;
    if (ignore) {
        const char* old = unit(w, ignore);
        unsigned id;
        int ox, oz, ofx, ofz;
        const unsigned char *oldyard, *newyard;
        if (!old || !(id = def_id(w, old))) return 0;
        if (*(unsigned*)(old + U_FLAGS) & 0x20000000u) {
            /* DISASSEMBLED: 0x47D17C clears the yard bit for every old yard
               byte with bit 0. The candidate's bit 0 is read ONLY by
               0x47D530's yard-bit test. Project that post-clear test into a
               bounded private yard; the engine grid and definitions stay
               untouched. Contested footprints can re-stamp another unit
               during clear, so they cannot establish this projection. */
            if (*(unsigned*)(old + U_FLAGS) & 0x0C000000u) return 0;
            ox = *(short*)(old + 0x76); oz = *(short*)(old + 0x78);
            if (!footprint(w->defs + id * D_STRIDE, &ofx, &ofz) ||
                ofx != *(short*)(old + 0x7E) || ofz != *(short*)(old + 0x80) ||
                ox < 0 || oz < 0 || ox + ofx > w->w || oz + ofz > w->h) return 0;
            oldyard = *(const unsigned char* const*)(w->defs + id * D_STRIDE + 0x14E);
            newyard = *(const unsigned char* const*)(d + 0x14E);
            if (!oldyard || !newyard) return 0;
            memcpy(copy, d, sizeof copy); memcpy(yard, newyard, (size_t)fx * fz);
            for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++) {
                int rx = x + dx - ox, rz = z + dz - oz;
                if (rx >= 0 && rz >= 0 && rx < ofx && rz < ofz &&
                    (oldyard[rz * ofx + rx] & 1)) yard[dz * fx + dx] &= ~1u;
            }
            *(unsigned char**)(copy + 0x14E) = yard;
            d = (char*)copy;
        }
    }
    /* 0x47D554 exempts exactly the old unit number from occupancy checks. */
    ok = ((GridTest)0x0047D2E0u)(d, (unsigned)x | (unsigned)z << 16, ignore,
                                w->ta + 0x1B63 + w->player * 0x14B) & 1;
    if (ok && height) *height = ((SiteHeight)0x0047C780u)() & 255;
    return ok;
}

static int distinct(const World* w, Choice* c, int fx, int fz);
static int queued(const World* w, const Choice* c);
static int plans_block(const World* w, const Choice* c);

static int site(const World* w, unsigned type, const int seed[3], const int raw[3], int radius,
                int legal, unsigned ignore, Choice* out)
{
    int fx, fz, dx, dz, base_x = seed[0] >> 20, base_z = seed[2] >> 20;
    int best = 0, bx = 0, bz = 0, height = 0, k = kind(w->defs + type * D_STRIDE);
    double distance = 1e100;
    if (!k || !footprint(w->defs + type * D_STRIDE, &fx, &fz)) return 0;
    for (dz = -radius; dz <= radius; dz++) for (dx = -radius; dx <= radius; dx++) {
        int cx = base_x + dx, cz = base_z + dz, x = cx - fx / 2, z = cz - fz / 2;
        int h = 0, count;
        double tx = cx - raw[0] / 1048576.0 + 0.5;
        double tz = cz - raw[2] / 1048576.0 + 0.5;
        double dist = tx * tx + tz * tz;
        if (x < 0 || z < 0 || x + fx > w->w || z + fz > w->h ||
            x * 16 + fx * 8 > 32767 || z * 16 + fz * 8 > 32767) continue;
        count = k == 1 ? coverage(w, x, z, fx, fz) : 1;
        if (!count || count < best || (count == best && dist >= distance)) continue;
        if (legal) {
            Choice candidate = {0};
            int taken = !fits(w, type, x, z, ignore, &h);
            if (taken && !site_known(w, type, x, z)) continue;
            candidate.type = type; candidate.kind = k; candidate.x = x; candidate.z = z;
            candidate.height = h; candidate.target = ignore;
            /* WHETHER A PLAN IS QUEUED HERE IS DECIDED BEFORE WHETHER ITS GROUND IS FREE. Ground
               the engine's site test refuses -- a unit standing on the site or crossing it, a
               wreck -- or another plan's yard is this click's only as the site of a plan
               already queued, and then it is that plan's whatever stands on it: the click
               reaches the repeat and the Shift-cancel (click), never a native order in the
               plan's place. A free site with no plan in the way is decided as before. */
            if (taken || plans_block(w, &candidate)) {
                if (k == 1 && !ignore && !distinct(w, &candidate, fx, fz)) continue;
                if (!queued(w, &candidate)) continue;
                if (taken) h = blocked_height(w, type, x, z);
            }
        } else if (k == 2 && !fits(w, type, x, z, ignore, &h)) continue;
        best = count; distance = dist; bx = x; bz = z; height = h;
    }
    if (!best) return 0;
    out->type = type; out->kind = k; out->x = bx; out->z = bz; out->height = height;
    return 1;
}

/* A closed, explored metal component is a patch. A component reaching the
   finite window is broad metal ground; unexplored neighbors cannot prove
   closure. No unseen metal byte is consulted. Eight neighbors keep diagonal
   deposit cells in one patch. The arena bound is independent of map size. */
static int distinct(const World* w, Choice* c, int fx, int fz)
{
    int x, z, sx = -1, sz = -1, left, top;
    unsigned head = 0, tail = 0;
    int boundary = 0, edges = 0;
    for (z = c->z; z < c->z + fz && sx < 0; z++) for (x = c->x; x < c->x + fx; x++)
        if (metal(w, x, z) > w->threshold) { sx = x; sz = z; break; }
    if (sx < 0) return 0;
    left = sx - PATCH_R; top = sz - PATCH_R;
    memset(s_patch_seen, 0, sizeof s_patch_seen);
    s_patch_queue[tail++] = PATCH_R * PATCH_SIDE + PATCH_R;
    s_patch_seen[PATCH_R * PATCH_SIDE + PATCH_R] = 1;
    c->anchor_x = sx; c->anchor_z = sz;
    while (head < tail) {
        unsigned idx = s_patch_queue[head++];
        int rx = idx % PATCH_SIDE, rz = idx / PATCH_SIDE, dx, dz;
        if (!rx || !rz || rx == PATCH_SIDE - 1 || rz == PATCH_SIDE - 1) return 0;
        if (left + rx == 0) edges |= 1;
        if (left + rx == w->w - 1) edges |= 2;
        if (top + rz == 0) edges |= 4;
        if (top + rz == w->h - 1) edges |= 8;
        for (dz = -1; dz <= 1; dz++) for (dx = -1; dx <= 1; dx++) {
            unsigned next = (unsigned)(rz + dz) * PATCH_SIDE + rx + dx;
            int wx = left + rx + dx, wz = top + rz + dz;
            if (s_patch_seen[next] || wx < 0 || wz < 0 || wx >= w->w || wz >= w->h) continue;
            s_patch_seen[next] = 1;
            if (!discovered(w, wx, wz)) return 0;
            if (metal(w, wx, wz) <= w->threshold) { boundary = 1; continue; }
            if (tail >= PATCH_CAP) return 0;
            s_patch_queue[tail++] = (unsigned short)next;
            if (wz < c->anchor_z || (wz == c->anchor_z && wx < c->anchor_x))
                c->anchor_x = wx, c->anchor_z = wz;
        }
    }
    /* The edge of the map alone cannot distinguish a small uniform metal
       map from a deposit. Require a discovered background cell as well. */
    return boundary && (edges & 3) != 3 && (edges & 12) != 12;
}

static int resolve(const char* u, int command, const char* target, const int pos[3])
{
    int index = -1;
    const unsigned char* action = ((Resolve)0x0043F0E0u)(&index, command,
                                (void*)u, (void*)target, pos);
    /* 0x4401E3 returns the output pointer with byte zero on refusal;
       the native caller 0x48D0AF accepts only a nonzero action. */
    return action && *action ? *action : -1;
}

static void position(const World* w, const Choice* c, int pos[3])
{
    int fx, fz;
    footprint(w->defs + c->type * D_STRIDE, &fx, &fz);
    pos[0] = (c->x * 16 + fx * 8) * 65536;
    pos[1] = c->height * 65536;
    pos[2] = (c->z * 16 + fz * 8) * 65536;
}

static int snapped_site(const World* w, unsigned type, const int raw[3], int radius,
                        int legal, Choice* out)
{
    Choice c = {0};
    if (!site(w, type, raw, raw, radius, legal, 0, &c)) return 0;
    if (c.kind == 1) {
        Choice stable = {0};
        int fx, fz, next[3];
        footprint(w->defs + type * D_STRIDE, &fx, &fz);
        position(w, &c, next);
        if (c.x + fx / 2 != (raw[0] >> 20) || c.z + fz / 2 != (raw[2] >> 20)) {
            /* A search boundary can expose only part of a deposit. Both
               modes require the chosen coverage target to remain best in
               the footprint-sized neighborhood, using the raw mouse tie.
               Occupancy cannot turn a partial target into a complete one. */
            if (!site(w, type, next, raw, fx > fz ? fx : fz, 0, 0, &stable) ||
                stable.x != c.x || stable.z != c.z) return 0;
        }
    }
    *out = c;
    return 1;
}

static int build_node(const World* w, const char* n)
{
    const char* first = *(const char* const*)0x00512344u;
    const char* last = *(const char* const*)0x00512348u;
    uintptr_t bytes;
    unsigned action = (unsigned char)n[4], type = *(unsigned short*)(n + 0x36);
    if (!first || (uintptr_t)last < (uintptr_t)first) return 0;
    bytes = (uintptr_t)last - (uintptr_t)first;
    /* The loader's descriptor array is fixed for the level, stride 0x19.
       Capability bit 0 identifies placed build orders, not factory counts. */
    return bytes % 0x19u == 0 && action < bytes / 0x19u &&
        (*(unsigned*)(first + action * 0x19u + 0x0C) & 1u) &&
        type && type < w->ndefs && *(int*)(n + 0x22) > 0 && *(int*)(n + 0x2A) > 0;
}

static int plans_block(const World* w, const Choice* c)
{
    const char* d = w->defs + c->type * D_STRIDE;
    const char *primary = NULL, *target = c->target ? unit(w, c->target) : NULL;
    const unsigned char* yard = *(const unsigned char* const*)(d + 0x14E);
    int fx, fz, shift = w->shift;
    unsigned i;
    int pos[3];
    if (!footprint(d, &fx, &fz) || !yard) return 1;
    position(w, c, pos);
    if (!shift) for (i = w->first; i <= w->last; i++) {
        const char* u = unit(w, i);
        if (u && selected(w, u) && can_build(w, u, c->type) &&
            (!c->target || (target && resolve(u, 12, target, pos) >= 0))) { primary = u; break; }
    }
    for (i = w->first; i <= w->last; i++) {
        const char *u = unit(w, i), *n;
        unsigned steps = 0;
        int ordered;
        if (!u || *(unsigned char*)(u + U_OWNER) != w->player) continue;
        ordered = primary && selected(w, u) &&
            ((can_build(w, u, c->type) && (!c->target || resolve(u, 12, target, pos) >= 0)) ||
             (can_assist(w, u) && resolve(u, 7, primary, pos) >= 0));
        for (n = *(const char* const*)(u + U_ORDERS); n && steps++ < 16384;
             n = *(const char* const*)(n + 0x4A)) {
            const char* od;
            const unsigned char* oy;
            int ofx, ofz, ox, oz, x, z;
            if (*(const char* const*)(n + 0x0E) != u) break;
            if (!build_node(w, n)) continue;
            if (ordered &&
                !(*(unsigned*)(n + 0x42) & 4u)) continue;
            od = w->defs + *(unsigned short*)(n + 0x36) * D_STRIDE;
            if (od[0x22F] || !footprint(od, &ofx, &ofz) ||
                !(oy = *(const unsigned char* const*)(od + 0x14E))) continue;
            ox = (int)(((int64_t)*(int*)(n + 0x22) - ofx * 524288 + 524288) >> 20);
            oz = (int)(((int64_t)*(int*)(n + 0x2A) - ofz * 524288 + 524288) >> 20);
            for (z = c->z > oz ? c->z : oz; z < c->z + fz && z < oz + ofz; z++)
                for (x = c->x > ox ? c->x : ox; x < c->x + fx && x < ox + ofx; x++) {
                    unsigned a = yard[(z - c->z) * fx + x - c->x];
                    unsigned b = oy[(z - oz) * ofx + x - ox];
                    if (((a & 4u) && (b & 6u)) || ((b & 4u) && (a & 6u))) return 1;
                }
        }
    }
    return 0;
}

static int geo_anchor(const World* w, unsigned type, int x, int z, int* ax, int* az)
{
    const char* d = w->defs + type * D_STRIDE;
    const unsigned char* yard = *(const unsigned char* const*)(d + 0x14E);
    const char* defs = *(const char* const*)(w->ta + OFF_FEATDEF);
    unsigned count = *(unsigned*)(w->ta + OFF_FEATCOUNT);
    int fx, fz, dx, dz, found = 0;
    if (!yard || !defs || count > 0xFFFBu || !footprint(d, &fx, &fz) ||
        x < 0 || z < 0 || x + fx > w->w || z + fz > w->h) return 0;
    for (dz = 0; dz < fz; dz++) for (dx = 0; dx < fx; dx++) {
        const char* cell;
        unsigned id;
        int cx = x + dx, cz = z + dz;
        if (!(yard[dz * fx + dx] & 0x80u) || !discovered(w, cx, cz)) continue;
        cell = w->grid + ((size_t)cz * w->w + cx) * FT_STRIDE;
        id = *(const unsigned short*)(cell + 8);
        /* DISASSEMBLED 0x47D6C7..0x47D6E4: FFFE cells carry dz/dx
           offsets to their feature anchor. Bound both before subtraction. */
        if (id == 0xFFFEu) {
            int ox = (unsigned char)cell[11], oz = (unsigned char)cell[10];
            if (ox > cx || oz > cz) continue;
            cx -= ox; cz -= oz;
            if (!discovered(w, cx, cz)) continue;
            cell = w->grid + ((size_t)cz * w->w + cx) * FT_STRIDE;
            id = *(const unsigned short*)(cell + 8);
        }
        if (id >= count || id >= 0xFFFBu || !(defs[id * FD_STRIDE + 0xFE] & 0x20u)) continue;
        /* One footprint touching several distinct vents has no unique
           cancellation identity; do not guess which reservation it means. */
        if (found && (*ax != cx || *az != cz)) return 0;
        *ax = cx; *az = cz; found = 1;
    }
    return found;
}

static int same_plan(const World* w, const Choice* c, const char* node)
{
    unsigned t = *(unsigned short*)(node + 0x36);
    int x, z, fx, fz;
    if (!build_node(w, node) || kind(w->defs + t * D_STRIDE) != c->kind ||
        !footprint(w->defs + t * D_STRIDE, &fx, &fz)) return 0;
    x = ((*(int*)(node + 0x22) >> 16) - fx * 8) / 16;
    z = ((*(int*)(node + 0x2A) >> 16) - fz * 8) / 16;
    if (c->kind == 2) {
        int ax, az, bx, bz;
        return geo_anchor(w, c->type, c->x, c->z, &ax, &az) &&
            geo_anchor(w, t, x, z, &bx, &bz) && ax == bx && az == bz;
    } else if (c->target) {
        int pos[3];
        position(w, c, pos);
        return llabs((int64_t)*(int*)(node + 0x22) - pos[0]) <= 1048576 &&
               llabs((int64_t)*(int*)(node + 0x2A) - pos[2]) <= 1048576;
    } else {
        Choice other = {0};
        other.x = x; other.z = z;
        return x >= 0 && z >= 0 && x + fx <= w->w && z + fz <= w->h &&
            distinct(w, &other, fx, fz) && other.anchor_x == c->anchor_x &&
            other.anchor_z == c->anchor_z;
    }
}

static int find_cancel(const World* w, const char* u, const Choice* c, Cancel* out)
{
    const char *node, *previous = NULL, *target = c->target ? unit(w, c->target) : NULL;
    unsigned steps = 0, id = def_id(w, u);
    if (!id || !(*(unsigned*)(w->defs + id * D_STRIDE + D_FLAGS) & 0x40u)) return 0;
    for (node = *(const char* const*)(u + U_ORDERS); node && steps++ < 16384;
         previous = node, node = *(const char* const*)(node + 0x4A)) {
        int pos[3];
        if (*(const char* const*)(node + 0x0E) != u) break;
        if (!same_plan(w, c, node) || (*(unsigned*)(node + 0x42) & 0x40000u)) continue;
        memcpy(pos, node + 0x22, sizeof pos);
        out->build = node; out->reclaim = NULL;
        if (c->target) {
            if (!target || !previous || node == *(const char* const*)(u + U_ORDERS) ||
                (*(unsigned*)(previous + 0x42) & 0x40000u) ||
                *(const char* const*)(previous + 0x16) != target ||
                resolve(u, 12, target, pos) != (unsigned char)previous[4] ||
                memcmp(previous + 0x22, pos, sizeof pos)) continue;
            /* DISASSEMBLED 0x43A1F0: an unstarted, non-head build with no
               StopBuilding flag or owned object cannot call a handler/script/
               object destructor. Exact removal sets 0x10000 for non-heads,
               skipping 0x489800 too. Only this build's reference is unlinked;
               the preceding reclaim stays live until its own removal. */
            if (((unsigned char)node[6] & 2u) || (*(unsigned*)(node + 0x42) & 0x400000u) ||
                *(const char* const*)(node + 0x52)) continue;
            out->reclaim = previous;
        }
        return 1;
    }
    return 0;
}

static int queued(const World* w, const Choice* c)
{
    unsigned i;
    int found = 0;
    for (i = w->first; i <= w->last; i++) {
        const char *u = unit(w, i), *node;
        unsigned steps = 0;
        if (!u || *(unsigned char*)(u + U_OWNER) != w->player) continue;
        /* The live unit owns this heap chain; its sole mutator/destructor is
           this thread. The walk is bounded even for a malformed cycle. */
        for (node = *(const char* const*)(u + U_ORDERS); node && steps++ < 16384;
             node = *(const char* const*)(node + 0x4A)) {
            if (*(const char* const*)(node + 0x0E) != u) break;
            if (same_plan(w, c, node)) {
                Cancel cancel;
                found |= TAGPU_RESOURCE_QUEUED;
                if (selected(w, u) && find_cancel(w, u, c, &cancel))
                    return found | TAGPU_RESOURCE_CANCEL;
                break;
            }
        }
    }
    return found;
}

static unsigned unqueue(const World* w, const Choice* c)
{
    unsigned i, count = 0;
    for (i = w->first; i <= w->last; i++) {
        const char* u = unit(w, i);
        Cancel cancel;
        if (!u || !selected(w, u) || !find_cancel(w, u, c, &cancel)) continue;
        /* DISASSEMBLED 0x439F80 removes this exact node, never appends or
           cancels a nearby site. The only pointer kept across its call is
           the paired reclaim whose lifetime find_cancel proves above. */
        ((RemoveOrder)0x00439F80u)((void*)u, cancel.build);
        if (cancel.reclaim) ((RemoveOrder)0x00439F80u)((void*)u, cancel.reclaim);
        count++;
    }
    return count;
}

static int quick_choice(const World* w, Choice* chosen)
{
    unsigned i, t, target_id = *(unsigned short*)(w->ta + 0x2CBA);
    unsigned char available[TAGPU_LIM_TYPES] = {0};
    const char* target = unit(w, target_id);
    int desired = 0;
    float old = 0;
    Choice best[3] = {{0}};
    const TagpuResourcePolicy* p = tagpu_profiles_policy();
    if (!s_raw_valid || *(unsigned char*)(w->ta + M_MODE) != 1 ||
        !(*(unsigned char*)(w->ta + M_REGION) & 2) || !tagpu_profiles_quick() ||
        (fake_GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
    if (target) {
        unsigned id = def_id(w, target);
        if (!id || *(unsigned char*)(target + U_OWNER) != w->player ||
            *(float*)(target + 0x104) != 0 || !(desired = kind(w->defs + id * D_STRIDE))) return 0;
        old = strength(w->defs + id * D_STRIDE, desired);
    } else {
        unsigned feature = *(unsigned short*)(w->ta + 0x2CBC);
        unsigned count = *(unsigned*)(w->ta + OFF_FEATCOUNT);
        const char* defs = *(const char* const*)(w->ta + OFF_FEATDEF);
        if (count <= 0xFFFBu && feature < count && defs && (*(unsigned short*)(defs + feature * FD_STRIDE + 0xFE) & 0x80u)) return 0;
    }
    for (i = w->first; i <= w->last; i++) {
        const char* u = unit(w, i);
        unsigned id, j;
        const Menu* m;
        if (!u || !selected(w, u) || !(id = def_id(w, u))) continue;
        if (!(*(unsigned*)(w->defs + id * D_STRIDE + D_FLAGS) & 0x40u)) continue;
        if (target && resolve(u, 12, target, s_raw) < 0) continue;
        if (!(m = build_menu(w, id))) continue;
        for (j = 0; j < m->n; j++) if (m->types[j] && m->types[j] < w->ndefs) available[m->types[j]] = 1;
    }
    memset(chosen, 0, sizeof *chosen);
    for (t = 1; t < w->ndefs; t++) if (available[t]) {
        Choice c = {0};
        char* d = w->defs + t * D_STRIDE;
        int k = kind(d), fx, fz;
        float value;
        if (!k || (desired && k != desired) || (value = strength(d, k)) <= old) continue;
        if (best[k].type && value <= best[k].strength) continue;
        if (!footprint(d, &fx, &fz)) continue;
        if (target) {
            int pos[3];
            memcpy(pos, target + U_XFIX, sizeof pos);
            c.x = ((pos[0] >> 16) - fx * 8 + 8) / 16;
            c.z = ((pos[2] >> 16) - fz * 8 + 8) / 16;
            c.type = t; c.kind = k; c.target = target_id;
            /* the rule of site(): an upgrade already queued here is decided before the ground */
            if (!fits(w, t, c.x, c.z, *(unsigned short*)(target + U_INDEX), &c.height)) {
                if (!site_known(w, t, c.x, c.z) || !queued(w, &c)) continue;
                c.height = blocked_height(w, t, c.x, c.z);
            }
        } else {
            if (!snapped_site(w, t, s_raw, p->radius, 1, &c)) continue;
            if (k == 1 && !distinct(w, &c, fx, fz)) continue;
        }
        c.strength = value; c.target = target ? target_id : 0;
        c.queued = queued(w, &c);
        if (!c.queued && plans_block(w, &c)) continue;
        best[k] = c;
    }
    *chosen = best[1].type ? best[1] : best[2];
    if (best[1].type && best[2].type) {
        int pos[3], oldpos[3];
        int64_t dx, dz, d1, d2;
        position(w, &best[2], pos); position(w, &best[1], oldpos);
        dx = (pos[0] - (int64_t)s_raw[0]) >> 16; dz = (pos[2] - (int64_t)s_raw[2]) >> 16;
        d1 = dx * dx + dz * dz;
        dx = (oldpos[0] - (int64_t)s_raw[0]) >> 16; dz = (oldpos[2] - (int64_t)s_raw[2]) >> 16;
        d2 = dx * dx + dz * dz;
        if (d1 < d2) *chosen = best[2];
    }
    if (!chosen->type) return 0;
    chosen->queued = queued(w, chosen);
    return 1;
}

void tagpu_resource_level_end(void) { s_raw_valid = 0; }

void tagpu_resource_mouse(void)
{
    char* ta = *(char**)TA_MAIN_PP;
    if (!s_armed || !ta) { s_raw_valid = 0; return; }
    tagpu_profiles_begin_match();
    memcpy(s_raw, ta + M_POINT, sizeof s_raw);
    s_raw_valid = 1;
}

static int __cdecl manual_cursor(void* frame)
{
    World w;
    Choice c = {0};
    unsigned type;
    int radius, pos[3];
    (void)frame;
    if (!s_raw_valid || !world(&w) || *(unsigned char*)(w.ta + M_MODE) != 14) return 0;
    /* Restore the raw point on every evaluation, including a stationary Alt
       change. The capture is made after the viewport's mouse repair. */
    memcpy(w.ta + M_POINT, s_raw, sizeof s_raw);
    type = *(unsigned short*)(w.ta + M_TYPE);
    radius = tagpu_profiles_radius();
    if (!type || type >= w.ndefs || !radius || (fake_GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
    if (snapped_site(&w, type, s_raw, radius, 0, &c)) {
        position(&w, &c, pos);
        pos[1] = s_raw[1];
        memcpy(w.ta + M_POINT, pos, sizeof pos);
    }
    return 0;
}

static int existing_guard(const char* u, const char* target, int action)
{
    const char* n;
    unsigned steps = 0;
    for (n = *(const char* const*)(u + U_ORDERS); n && steps++ < 16384;
         n = *(const char* const*)(n + 0x4A))
        if (*(unsigned char*)(n + 4) == action && *(const char* const*)(n + 0x16) == target) return 1;
    return 0;
}

static int click(void* frame, int right)
{
    World w;
    Choice c;
    const int* record = ((const int* const*)frame)[1];
    const char* primary[TAGPU_LIM_UNITS];
    unsigned char actions[TAGPU_LIM_UNITS];
    int reclaims[TAGPU_LIM_UNITS];
    unsigned np = 0, i, helper = 0;
    int shift, pos[3];
    if (!s_armed || !world(&w)) return 0;
    if (!right && *(unsigned char*)(w.ta + M_MODE) == 14) {
        manual_cursor(NULL);
        ((void (__stdcall*)(void))0x004197D0u)();
        return 0;
    }
    if ((*(int*)(w.ta + 0x37EFA) != 0) != right) return 0;
    /* The queued mouse record owns its modifier state. A later key release
       must not change which existing plans this click will preserve. */
    w.shift = (record[2] & 4) != 0;
    {
        unsigned scratch0 = *(unsigned*)0x0051E684u, scratch1 = *(unsigned*)0x0051E688u;
        int have = quick_choice(&w, &c);
        *(unsigned*)0x0051E684u = scratch0; *(unsigned*)0x0051E688u = scratch1;
        if (!have) return 0;
    }
    if (c.queued) {
        if (w.shift && (c.queued & TAGPU_RESOURCE_CANCEL)) {
            unsigned removed = unqueue(&w, &c);
            tagpu_logf("resource: unqueued type=%u site=%d,%d builders=%u upgrade=%u",
                       c.type, c.x, c.z, removed, c.target);
            return 1;
        }
        tagpu_logf("resource: already queued type=%u site=%d,%d", c.type, c.x, c.z);
        return 1;
    }
    position(&w, &c, pos);
    shift = w.shift;
    for (i = w.first; i <= w.last; i++) {
        const char* u = unit(&w, i);
        if (u && selected(&w, u) && can_build(&w, u, c.type) &&
            (!c.target || resolve(u, 12, unit(&w, c.target), pos) >= 0)) {
            if (np >= TAGPU_LIM_UNITS) return 0;
            const char* d = w.defs + def_id(&w, u) * D_STRIDE;
            unsigned char action = 0;
            ((Action)0x00438760u)(&action, (*(unsigned*)(d + D_FLAGS) & 0x800u) ?
                "VTOL_MOBILEBUILD" : "MOBILEBUILD");
            if (!action) return 0;
            primary[np] = u; actions[np] = action;
            reclaims[np] = c.target ? resolve(u, 12, unit(&w, c.target), pos) : -1;
            np++;
        }
    }
    if (!np) return 0;
    for (i = 0; i < np; i++) {
        const char* u = primary[i];
        if (c.target) {
            const char* target = unit(&w, c.target);
            ((Order)0x0043ADC0u)(reclaims[i], shift, (void*)u, (void*)target, pos, 0, 0);
        }
        /* Cancellation has its own validated branch. New plans append or
           replace through the native constructor; the Shift toggle root
           would otherwise delete a different site within 16 world units. */
        ((Order)0x0043ADC0u)(actions[i], shift || c.target, (void*)u, NULL, pos, c.type, 0);
    }
    for (i = w.first; i <= w.last; i++) {
        const char *u = unit(&w, i), *main;
        unsigned j;
        int guard;
        if (!u || !selected(&w, u) || !can_assist(&w, u)) continue;
        for (j = 0; j < np && u != primary[j]; j++) {}
        if (j < np) continue;
        main = primary[helper++ % np];
        guard = resolve(u, 7, main, pos);
        if (guard < 0) continue;
        if (shift) {
            for (j = 0; j < np && !existing_guard(u, primary[j], guard); j++) {}
            if (j < np) continue;
        }
        ((Order)0x0043AFC0u)(guard, shift, (void*)u, (void*)main, pos, 0, 0);
    }
    tagpu_logf("resource: quick type=%u site=%d,%d builders=%u upgrade=%u shift=%d",
               c.type, c.x, c.z, np, c.target, shift);
    return 1;
}
static int __cdecl left_click(void* frame) { return click(frame, 0); }
static int __cdecl right_click(void* frame) { return click(frame, 1); }

void tagpu_resource_packet(TAGPU_PACKET* p)
{
    World w;
    Choice c;
    int fx, fz, have;
    unsigned scratch0, scratch1;
    if (!s_armed || !p->in_game || !world(&w)) return;
    scratch0 = *(unsigned*)0x0051E684u; scratch1 = *(unsigned*)0x0051E688u;
    have = quick_choice(&w, &c);
    *(unsigned*)0x0051E684u = scratch0; *(unsigned*)0x0051E688u = scratch1;
    if (!have) return;
    footprint(w.defs + c.type * D_STRIDE, &fx, &fz);
    p->cursor_mode = 14; p->build_unit_id = (unsigned short)c.type;
    p->region_flags = (p->region_flags & ~0x48u) | 0x40u;
    p->resource_queued = c.queued;
    p->build_rect[0] = c.x * 16; p->build_rect[1] = c.height; p->build_rect[2] = c.z * 16;
    p->build_rect[3] = (c.x + fx) * 16; p->build_rect[4] = c.height; p->build_rect[5] = (c.z + fz) * 16;
}

int tagpu_resource_live(void) { return s_armed; }

void tagpu_resource_init(void)
{
    static const unsigned char build[] = {0x83,0xEC,0x14,0x8B,0x15,0xE8,0x1D,0x51,0x00};
    static const unsigned char left[] = {0x51,0xA1,0xE8,0x1D,0x51,0x00};
    static const unsigned char right[] = {0x51,0x8B,0x0D,0xE8,0x1D,0x51,0x00};
    if (!tagpu_vpwide_mouse_world_live()) {
        tagpu_logf("resource: raw mouse coordinate producer unavailable; not armed"); return;
    }
    if (!tagpu_detour_bytes_ok(0x004197D0u, build, sizeof build) ||
        !tagpu_detour_bytes_ok(0x00498F70u, left, sizeof left) ||
        !tagpu_detour_bytes_ok(0x00499100u, right, sizeof right)) {
        tagpu_logf("resource: incompatible input/placement sites; not armed"); return;
    }
    if (tagpu_detour_observe(0x004197D0u, build, sizeof build, manual_cursor, NULL) &&
        tagpu_detour_gate(0x00498F70u, left, sizeof left, left_click, 4) &&
        tagpu_detour_gate(0x00499100u, right, sizeof right, right_click, 4)) {
        s_armed = 1;
        tagpu_logf("resource: input and placement hooks armed");
    } else tagpu_logf("resource: hook installation failed; not armed");
}
