#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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
#include "tagpu_worldq.h"
#include "tasim_snap.h"
#include "winapi_hooks.h"

/* Quick Build and the build cursor's snap in the game: their hooks, gates, logs and the packet's ghost.
   The rules themselves are tasim_snap.c's, over the engine's WORLDQ (tagpu_worldq.c) for the watched
   player, as the in-game wiki plays the same functions over its own world. DISASSEMBLED: 0x419670 uses
   the order roots and reads no build list (0x41975E..0x4197A9); Quick Build's candidates are each
   constructor's build menu (tagpu_buildmenu.h, read below for the view's WqMenuFn). The input and
   placement contracts are recorded in resource-snapping.md and exe-reverse-engineering.md. All engine
   access belongs to the input/publish thread, between simulation iterations. No pointer is retained
   across engine calls that can tear a level down. */
#define D_STRIDE 0x249u
#define D_BASE 0x1439B
#define U_FLAGS 0x110
#define D_FLAGS 0x241
#define M_POINT 0x2CAA
#define M_MODE 0x2CC3
#define M_TYPE 0x2CC4
#define M_REGION 0x2CC6

_Static_assert(TAGPU_RESOURCE_QUEUED == SNAP_QUEUED && TAGPU_RESOURCE_CANCEL == SNAP_CANCEL,
               "the packet's plan bits are the rule's");
_Static_assert(TAGPU_LIM_UNITS <= SNAP_UNITS_MAX && TAGPU_LIM_TYPES <= WQ_TYPES_MAX,
               "the rule's tables hold a seat's units and every type");

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

/* The build menu's view of the world: the main block, the defs and their count, the seat whose side's pages
   are read. */
typedef struct {
    char *ta, *defs;
    unsigned ndefs;
    int player;
} World;

/* UI state, game-thread owned: the engine simulation never reads it. The rule's scratch serves one
   call at a time, and every call is on this thread. */
static int s_armed, s_raw[3], s_raw_valid;
static SNAP_SCRATCH s_scr;
static uint16_t s_sel[SNAP_UNITS_MAX];

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
   first use for the side of the seat that asks, and rebuilt when a seat of another side asks.
   THE TABLE'S LIFETIME: it holds type numbers only, and every number read out of it is bounded
   against the live def count before it addresses a def, so even a table kept past its level
   costs a wrong candidate, never a read outside the defs. */
typedef struct { unsigned short* types; unsigned short n; unsigned char side, built; } Menu;
static Menu* s_menu;
static unsigned s_menuN, s_menuGen;
static uintptr_t s_menuDefs;

/* the build menu's world: in a level, the seat 0..9, the def count 1..TAGPU_LIM_TYPES */
static int menu_world(World* w, int seat)
{
    memset(w, 0, sizeof *w);
    if (!(w->ta = *(char**)TA_MAIN_PP)) return 0;
    w->player = seat;
    w->ndefs = *(unsigned*)(w->ta + OFF_UDEFCOUNT);
    w->defs = *(char**)(w->ta + D_BASE);
    return seat >= 0 && seat < 10 && w->ndefs && w->ndefs <= TAGPU_LIM_TYPES && w->defs;
}

/* ---- the build menu, Quick Build's candidates --------------------------------------------

   THE ORDERING THAT MAKES EVERY CALL BELOW SAFE: build_menu runs only inside the views this file
   opens for Quick Build -- from the left and right command gates 0x498F70 / 0x499100 and the
   packet fill -- on the GAME thread, in play, between two simulation steps and never inside an
   engine file call. That is the thread and the phase in which the engine itself calls the page
   loader 0x41ACE0, its GUI_Load 0x4AA8F0 and the TDF loader 0x4C2F60, which make these same calls;
   a puppet seat never gets here (tagpu_worldq_open refuses its view). The def array, the download
   records and the side table it reads are written by the loader before the first in-play step and
   freed by the teardown 0x491B60, which runs on this thread. The rule itself is tagpu_buildmenu.c;
   this is its engine half. */

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

/* Every type a press of the page reaches, armed or queued: the sidebar arms a type whose def +0x22F is 0
   (0x41AB7F) and hands any other to the factory's queue (0x41AC3C, 0x419B00) -- engine/ui.md, the build
   press. A factory's menu holds only the second kind, so Quick Build's armable rule would leave it empty. */
static int bm_pressable(void* ctx, unsigned type)
{
    return type && type < ((const World*)ctx)->ndefs;
}

/* The seat's side, read as 0x41ACE0 reads the controlled player's for the DL page (0x41AD61..0x41AD66):
   PlayerInfo at the player record +0x27, its byte +0x95. PlayerInfo objects live for the
   process. BOUND: below the sides loaded and the five records; 0xFF when out of bounds. */
static unsigned char seat_side(const World* w)
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
   0x4290F0 + 0x4BBC40 answers a size, else the seat's side's "<prefix>DL". BOUND: the probe's
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

/* The types builder `id`'s pages offer for `side` into out, ascending: the count, -1 when cap is short or the
   read's memory cannot be had. With `queued` the types a press queues count too (bm_pressable), else only those
   it arms. `say` logs each page that cannot be read; `pages` and `read` count the pages chosen and read. */
static int menu_offers(const World* w, unsigned id, unsigned char side, uint16_t* out, unsigned cap, int queued,
                       int say, unsigned* pages_out, unsigned* read_out)
{
    const char* d = w->defs + id * D_STRIDE;
    unsigned char pages[TAGPU_BM_PAGES], *offered;
    TAGPU_BM_ENGINE eng;
    TAGPU_BM_ENTRY* entries;
    unsigned np, nentries, p, t, read = 0, n = 0;
    np = tagpu_buildmenu_pages((*(const unsigned*)(d + D_FLAGS) & 0x80000000u) != 0,
                               *(const unsigned char*)(d + 0x22E), pages);
    entries = dl_entries(w, *(const unsigned short*)(d + 0x21E), &nentries);
    if (!(offered = (unsigned char*)calloc(w->ndefs, 1))) { free(entries); return -1; }
    eng.type_of = bm_type_of; eng.armable = queued ? bm_pressable : bm_armable; eng.ctx = (void*)w;
    eng.ndefs = w->ndefs;
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
        if (refused && say) tagpu_logf("resource: menu %.31s page %u: %s", d + 0x20, pages[p], refused);
    }
    free(entries);
    for (t = 1; t < w->ndefs; t++)
        if (offered[t]) {
            if (n == cap) { free(offered); return -1; }
            out[n++] = (uint16_t)t;
        }
    free(offered);
    if (pages_out) *pages_out = np;
    if (read_out) *read_out = read;
    return (int)n;
}

/* THE TWO THREADS. Quick Build calls this through build_menu on the GAME thread in play (the ordering
   above); ImpAI's level init calls it on the LOADER thread inside PLAYERS (0x4919C8), where the loader body
   0x4917D0 has already loaded the defs (0x42D2E0 at 0x4918CA) and the download records (0x42DCF0 at
   0x4918CF) and itself calls this same file layer (0x4290F0, 0x4C2F60, 0x4BBC40, 0x488B10, GUI_Load
   0x4AA8F0): the same thread and phase as the engine's own reads. The two never overlap: in play no loader
   runs, and during the load the game thread is in the load state, where no Quick Build gate is reached.
   BOUND: every bound the reader has -- page_bytes' 1 MiB, dl_entries' five entries, bm_type_of's type
   below ndefs, the side below the sides loaded and the five records (else only a builder's own pages). */
int tagpu_buildmenu_types(const char* ta, const char* defs, unsigned ndefs, unsigned id, unsigned char side,
                          uint16_t* out, unsigned cap)
{
    World w;
    const unsigned sides = ta ? *(const unsigned*)(ta + M_SIDES) : 0;
    if (!ta || !defs || !ndefs || ndefs > TAGPU_LIM_TYPES || !id || id >= ndefs) return -1;
    memset(&w, 0, sizeof w);
    w.ta = (char*)ta;
    w.defs = (char*)defs;
    w.ndefs = ndefs;
    w.player = -1;
    return menu_offers(&w, id, side < sides && side < SIDE_ROOM ? side : 0xFF, out, cap, 1, 0, NULL, NULL);
}

static void menu_read(const World* w, unsigned id, unsigned char side, Menu* m)
{
    const char* d = w->defs + id * D_STRIDE;
    LARGE_INTEGER t0, t1, hz;
    unsigned np = 0, read = 0, k, extractors = 0, geos = 0;
    int n;
    QueryPerformanceCounter(&t0);
    if (!(m->types = (unsigned short*)malloc(w->ndefs * sizeof *m->types))) return;
    n = menu_offers(w, id, side, m->types, w->ndefs, 0, 1, &np, &read);
    m->n = (unsigned short)(n > 0 ? n : 0);
    if (!m->n) { free(m->types); m->types = NULL; }
    for (k = 0; k < m->n; k++) {
        int kd = tagpu_worldq_def_kind(w->defs + m->types[k] * D_STRIDE);
        extractors += kd == WQ_KIND_MEX; geos += kd == WQ_KIND_GEO;
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&hz);
    tagpu_logf("resource: menu %.31s side=%d pages=%u read=%u offers=%u extractors=%u geos=%u ms=%.2f",
               d + 0x20, side == 0xFF ? -1 : (int)side, np, read, m->n, extractors, geos,
               (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)hz.QuadPart);
}

/* The menu of def `id` (a builder) for the seat's side, built on its first use against this
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
    side = seat_side(w);
    m = &s_menu[id];
    if (m->built && m->side == side) return m;
    free(m->types);
    memset(m, 0, sizeof *m);
    menu_read(w, id, side, m);
    m->side = side;
    m->built = 1;
    return m;
}

/* the view's WqMenuFn: the menu of builder type `id` for seat `seat`'s side, copied */
static int menu_types(int seat, unsigned id, uint16_t* out, int cap)
{
    World w;
    const Menu* m;
    if (!menu_world(&w, seat) || !(m = build_menu(&w, id))) return 0;
    if (m->n > cap) return -1;
    memcpy(out, m->types, (size_t)m->n * sizeof *out);
    return m->n;
}

/* the watched player's view, Quick Build's and the snap's */
static int view(WQ_ENGINE* e, WORLDQ* q)
{
    const char* ta = *(const char* const*)TA_MAIN_PP;
    return ta && tagpu_worldq_open(e, q, *(const unsigned char*)(ta + OFF_WATCHED), WQ_SITE_ENGINE, menu_types);
}

/* An alive unit that is not excluded at `slot`, of any owner. */
static const char* live_unit(const WQ_ENGINE* e, unsigned slot)
{
    const char* u;
    unsigned flags;
    if (!slot || slot >= e->nslots) return NULL;
    u = e->units + slot * UNIT_STRIDE;
    flags = *(const unsigned*)(u + U_FLAGS);
    return (flags & 0x10000000u) && !(flags & 0x4000u) ? u : NULL;
}

/* QUICK BUILD'S GATES AND INPUT, then the rule (tasim_snap_quick): nothing unless the raw point was
   captured, the cursor is in mode 1 and in the viewport (main+0x2CC6 bit 1), the profile's quick switch
   is on and Alt is up. The hovered unit (main+0x2CBA) is the input's target when it is alive, of any
   owner -- the rule refuses one that is not the player's -- and with none the hovered feature's def
   (main+0x2CBC, bounded by the feature def count) says whether it is reclaimable. The selection is the
   player's block's live units with +0x110 bit 4, ascending. */
static int quick_choice(const WQ_ENGINE* e, const WORLDQ* q, int shift, SNAP_QUICK_IN* in, SNAP_CHOICE* chosen)
{
    char* ta = e->ta;
    unsigned target_id = *(unsigned short*)(ta + 0x2CBA), i;
    const TagpuResourcePolicy* p = tagpu_profiles_policy();
    if (!s_raw_valid || *(unsigned char*)(ta + M_MODE) != 1 ||
        !(*(unsigned char*)(ta + M_REGION) & 2) || !tagpu_profiles_quick() ||
        (fake_GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
    memset(in, 0, sizeof *in);
    memcpy(in->raw, s_raw, sizeof in->raw);
    in->radius = p->radius;
    in->target = live_unit(e, target_id) ? (int)target_id : -1;
    if (in->target < 0) {
        unsigned feature = *(unsigned short*)(ta + 0x2CBC);
        unsigned count = *(unsigned*)(ta + OFF_FEATCOUNT);
        const char* defs = *(const char* const*)(ta + OFF_FEATDEF);
        in->feature_reclaimable = count <= 0xFFFBu && feature < count && defs &&
            (*(unsigned short*)(defs + feature * FD_STRIDE + 0xFE) & 0x80u);
    }
    for (i = e->first; i <= e->last && in->nbuilders < SNAP_UNITS_MAX; i++) {
        const char* u = live_unit(e, i);
        if (u && *(unsigned char*)(u + U_OWNER) == (unsigned)e->seat && (*(unsigned*)(u + U_FLAGS) & 0x10u))
            s_sel[in->nbuilders++] = (uint16_t)i;
    }
    in->builders = s_sel;
    in->shift = shift;
    return tasim_snap_quick(q, in, chosen, &s_scr);
}

static int shift_down(void) { return (fake_GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }

/* quick_choice with the scratch words 0x51E684 / 0x51E688, which the engine's site test and the build menu's
   page loads write, put back as they were for the engine's own reading */
static int quick_saved(const WQ_ENGINE* e, const WORLDQ* q, int shift, SNAP_QUICK_IN* in, SNAP_CHOICE* chosen)
{
    unsigned scratch0 = *(unsigned*)0x0051E684u, scratch1 = *(unsigned*)0x0051E688u;
    int have = quick_choice(e, q, shift, in, chosen);
    *(unsigned*)0x0051E684u = scratch0; *(unsigned*)0x0051E688u = scratch1;
    return have;
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
    WQ_ENGINE e;
    WORLDQ q;
    SNAP_CHOICE c;
    unsigned type;
    int radius;
    int32_t pos[3];
    (void)frame;
    if (!s_raw_valid || !view(&e, &q) || *(unsigned char*)(e.ta + M_MODE) != 14) return 0;
    /* Restore the raw point on every evaluation, including a stationary Alt
       change. The capture is made after the viewport's mouse repair. */
    memcpy(e.ta + M_POINT, s_raw, sizeof s_raw);
    type = *(unsigned short*)(e.ta + M_TYPE);
    radius = tagpu_profiles_radius();
    if (!type || type >= e.ndefs || !radius || (fake_GetAsyncKeyState(VK_MENU) & 0x8000)) return 0;
    if (tasim_snap_snapped(&q, NULL, (int)type, s_raw, radius, 0, &c, &s_scr)) {
        tasim_snap_position(&q, &c, pos);
        pos[1] = s_raw[1];
        memcpy(e.ta + M_POINT, pos, sizeof pos);
    }
    return 0;
}

static int click(void* frame, int right)
{
    WQ_ENGINE e;
    WORLDQ q;
    SNAP_QUICK_IN in;
    SNAP_CHOICE c;
    SNAP_COMMIT_RES res;
    const int* record = ((const int* const*)frame)[1];
    int shift;
    if (!s_armed || !view(&e, &q)) return 0;
    if (!right && *(unsigned char*)(e.ta + M_MODE) == 14) {
        manual_cursor(NULL);
        ((void (__stdcall*)(void))0x004197D0u)();
        return 0;
    }
    if ((*(int*)(e.ta + 0x37EFA) != 0) != right) return 0;
    /* The queued mouse record owns its modifier state. A later key release
       must not change which existing plans this click will preserve. */
    shift = (record[2] & 4) != 0;
    if (!quick_saved(&e, &q, shift, &in, &c)) return 0;
    tasim_snap_commit(&q, &in, &c, &res, &s_scr);
    switch (res.outcome) {
    case SNAP_UNQUEUED:
        tagpu_logf("resource: unqueued type=%u site=%d,%d builders=%u upgrade=%u",
                   (unsigned)c.type, c.x, c.z, (unsigned)res.removed, (unsigned)c.target);
        return 1;
    case SNAP_ALREADY:
        tagpu_logf("resource: already queued type=%u site=%d,%d", (unsigned)c.type, c.x, c.z);
        return 1;
    case SNAP_ORDERED:
        tagpu_logf("resource: quick type=%u site=%d,%d builders=%u upgrade=%u shift=%d",
                   (unsigned)c.type, c.x, c.z, (unsigned)res.built, (unsigned)c.target, shift);
        return 1;
    default:
        return 0;
    }
}
static int __cdecl left_click(void* frame) { return click(frame, 0); }
static int __cdecl right_click(void* frame) { return click(frame, 1); }

void tagpu_resource_packet(TAGPU_PACKET* p)
{
    WQ_ENGINE e;
    WORLDQ q;
    SNAP_QUICK_IN in;
    SNAP_CHOICE c;
    WQ_DEF d;
    if (!s_armed || !p->in_game || !view(&e, &q)) return;
    if (!quick_saved(&e, &q, shift_down(), &in, &c) || !q.ops->def(&q, c.type, &d)) return;
    p->cursor_mode = 14; p->build_unit_id = (unsigned short)c.type;
    p->region_flags = (p->region_flags & ~0x48u) | 0x40u;
    p->resource_queued = (uint8_t)c.queued;
    p->build_rect[0] = c.x * 16; p->build_rect[1] = c.height; p->build_rect[2] = c.z * 16;
    p->build_rect[3] = (c.x + d.fx) * 16; p->build_rect[4] = c.height; p->build_rect[5] = (c.z + d.fz) * 16;
}

int tagpu_resource_live(void) { return s_armed; }

/* What right_click would decide for the record being handled, without ordering: the same
   preconditions -- armed, a world, Interface Type 1 -- and the same quick_choice, with the
   build menu's scratch words saved around it as the gate and the packet fill save them. */
int tagpu_resource_quick_right(void)
{
    WQ_ENGINE e;
    WORLDQ q;
    SNAP_QUICK_IN in;
    SNAP_CHOICE c;
    if (!s_armed || !view(&e, &q) || *(int*)(e.ta + 0x37EFA) == 0) return 0;
    return quick_saved(&e, &q, shift_down(), &in, &c);
}

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
