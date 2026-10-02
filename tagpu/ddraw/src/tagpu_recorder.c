/* tagpu_recorder.c -- the demo recorder's capture and its event hooks, on the game thread
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S1 and S2a; the records are
   tagpu_recfmt.h's).

   WHERE IT RUNS. B2's stub at the sim step's call of the unit tick (0x4954ED, the only call of
   0x48AD30) calls tagpu_rec_capture before anything else: GameTime has just been incremented
   (0x4954C0) and every phase of the previous iteration has run, so a capture sees the world
   between two iterations, on the thread that owns it. A skirmish, the only game recorded, calls
   the step with 0 (0x49693C), so the step's network pump (0x4954C8) does not run. During play the
   game thread is the only writer of the engine records and of every SIM_STATE table the capture
   reads: the loader thread's writes end before the in-play entry (0x497F40 reads the loader's
   last store, bit 1 of main+0x38D75, before the first iteration). The event hooks (*The events*)
   record what the engine does between two captures, and only on that thread.

   WHAT IT READS, AND WHY EACH READ IS IN BOUNDS.
   - The unit array [begin, begin + nslots * 0x118): nslots is the engine's own count
     (main+0x14351), checked against the array's last-slot pointer (main+0x1435B) before any use.
     The array lives from the level load (0x4854A0) to the teardown's first free (0x485980 at
     0x491B95); the recording ends on entry to the teardown (tagpu_rec_level_end), before it.
   - A live unit's mover, 0x2F bytes (the creates' allocation, 0x4860C4): a LIFETIME. The creates
     store it in the unit's +0x00 and only the destructor frees it, nulling +0x00 and clearing the
     live bit in the same call (0x486DB1..0x486DCE), all on this thread; so between two iterations
     a live unit's +0x00 is NULL or its own mover.
   - A live unit's two order lists, from their heads (+0x5C, +0x60) through each node's next
     (+0x4A): an ORDERING. Every node is made, linked, unlinked and freed by game-thread code (the
     54 allocations of 0x56 bytes, the linkers, 0x43A1F0 and MEM_Free), so between two iterations
     a list reached from a live unit holds live nodes. The vtable test on each node (0x4FD2C8) is
     a sanity filter on the value, and a failure ends the recording (REC_END_ORDERS) rather than
     reading on; so does a list past REC_ORDERS_MAX nodes. A node's owned object (+0x52) is read
     once, for its vtable: the node owns it (its installers 0x4388D0, 0x438930, 0x438A00 and
     0x438AD0 delete the one before, the node's destructor deletes it), so it is live while the
     node holds it -- for a unit of a type 1 or 2 owner, as every unit of a skirmish is. A type-3
     owner's air proxy (vtable 0x4FD9E0), to which each install hands the object (slot 1,
     0x44EF90), deletes it itself (0x490A10, and its destructor 0x4909A0).
   - The ten player records, in the main block, and their PlayerInfo blocks: 0xB9 bytes the
     constructor allocates (0x463BE0, called at 0x41D98B for main's eleven) and nothing frees in
     main's records -- the destructor 0x463C40 runs only on a stack-local record (0x454538,
     0x454607), and the reset keeps the pointer (0x464508).
   - The declared tables, each bounded by its own declaration: a static array's rows, a live
     prefix clamped to them, a heap table's counted rows. Their shadows are taken at the start,
     and grown once for a heap table that grows (table_grow).
   - The feature grid, the wreck pool and the feature defs: *The features* below.
   - The fields of the main block and .data replay-globals.tsv lists, and the projectile pool and
     its count: *The globals and the projectiles* below; for the harness's dump, the main block
     and .data whole.
   - For the header, once: the unit-type definitions' names (UNITINFOCount of them), the weapons'
     names (the array's slots, tagpu_limits.h), the feature defs' names (their count), the
     skirmish settings and the game's options.
   Every other id row's pointer is never dereferenced: its ID is one more than its row in its
   table, by arithmetic (tagpu_recfmt.h), and a pointer outside the table becomes REC_ID_BAD,
   counted and logged. Each event hook's reads are stated beside it.

   WHAT IT WRITES: its own memory -- the shadow images, the ring and the dump file. The event
   hooks write nothing the simulation reads; the one site that changes a path, the network tests
   of the five fire functions, builds a packet on the fire function's own stack while a skirmish
   is recorded (*The events*). */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "tagpu_recorder.h"
#include "tagpu_cover.h"
#include "tagpu_recfmt.h"
#include "tagpu_replay_layout.h"
#include "tagpu_replay_globals.h"
#include "tagpu_simreg.h"
#include "tagpu_limits.h"
#include "tagpu_log.h"
#include "tagpu_reclaim.h"
#include "tagpu_opt.h"
#include "tagpu_detour.h"
#include "tagpu_patches.h"
#include "tagpu_puppet.h"
#include "tagpu_replay.h"
#include "crc32.h"
#include "git.h"

#define TA_MAINPP      0x00511DE8u
#define OFF_PLAYERS    0x1B63       /* the eleven player records, 0x14B apart                     */
#define PL_STRIDE      0x14B
#define PL_ALL         11
#define PL_SIGHT       0x7C         /* u8* the sight counters, one a cell, and their bytes at      */
#define PL_SIGHTLEN    0x88         /*   +0x88: the player init allocates +0x88 bytes there
                                         (0x464866..0x46487B), freed only at the level's end    */
#define PL_INFO        0x27         /* PlayerInfo*: +0x95 the side, +0x96 the logo (the saver's)  */
#define PL_NAME        0x2B         /* the long name the join stores (0x450BA3), 0x1E bytes         */
#define PL_NAMELEN     0x1E
#define PL_CONTROLLER  0x73
#define PL_ALLY        0x108        /* the alliance row, 11 bytes (the saver's `Alliances`)        */
#define PL_LIVE        0x144        /* u16: the player's live units; the destructor's decrement
                                       (0x486DFC) to 0 eliminates it (0x486E54)                  */
#define OFF_BUCKETS    0x1429F      /* the sort buckets, 10 bytes each                            */
#define OFF_BCOLS      0x142A3      /* u32 columns, and u32 rows at +0x142A7 (LoadMap, 0x482CA3)   */
#define OFF_BROWS      0x142A7
#define OFF_OFFMAP     0x142B7      /* the off-map bucket's address (compared at 0x412834)          */
#define BUCKET_STRIDE  10
#define OFF_NSLOTS     0x14351      /* u16: the unit array's slots, N * 10 + 1                     */
#define OFF_BEGIN      0x14357
#define OFF_LAST       0x1435B      /* the last slot's address, not one past it                   */
#define OFF_UCOUNT     0x1438F      /* UNITINFOCount                                              */
#define OFF_UDEFS      0x1439B
#define UD_STRIDE      0x249
#define UD_NAME        0x20         /* UnitName                                                   */
#define UD_NAMELEN     0x20
#define WPN_STRIDE     0x115        /* a weapon's record; its name is its first 0x20 bytes         */
#define WPN_NAMELEN    0x20
#define OFF_EACH       0x37EE6      /* u16: the units each player may have                        */
#define OFF_GAMETIME   0x38A47
#define OFF_GSTATE     0x391E9      /* GameingState*: +0 the mode (0x435100), +0x204 the TNT path  */
#define GS_TNT         0x204
#define OFF_CONTROLLED 0x2A42       /* u8: the player the local game commands                      */
#define OFF_VIEWED     0x2A43       /* u8: the player whose view it draws                         */
#define OFF_NPLAYERS   0x2A3C       /* u16: the player count the SKIRMISH screen set (0x47AF39)     */
#define OFF_LOSTYPE    0x14281      /* u16: bits 0-2 the options; bit 3 is the fog grid's own      */
#define OFF_CMDRDEATH  0x37EF6      /* u32: ActiveCommanderDeath                                  */
#define OFF_DIFFICULTY 0x37EEE      /* u32 (0x401441)                                             */
#define OFF_SETTINGS   0x29A0       /* the skirmish settings: +0x118 fixed starts, +0x11C the map  */
#define OFF_NPROJ      0x141F3      /* u32: the projectile pool's count                           */
#define OFF_PROJ       0x141F7      /* the projectile pool: TAGPU_LIM_PROJ records of 0x6B bytes   */
#define DATA_BEGIN     0x00501000u  /* .data, REC_DATA_BYTES from here (the section header)        */
#define OFF_MAPPED     0x14273      /* the explored map, REC_MAPPED_BYTES of the grid's columns
                                       and rows (LoadMap 0x483CF6..0x483D25)                    */
#define OFF_GRID_W     0x14233      /* the feature grid's columns and rows, i32                   */
#define OFF_GRID_H     0x14237
#define MODE_SKIRMISH  2
#define U_MOVER        0x00         /* the mover, a record of its own (the image's tail)           */
#define U_ORDERS       0x5C         /* the order lists' heads: the main list and the sub list      */
#define U_SUBORDERS    0x60
#define U_OWNER        0x96         /* the owning player's record                                 */
#define U_COB          0x9A
#define U_WATER        0x10A        /* the water state, a dword: 0x43DB50 alone writes it          */
#define U_STATEBYTE    0x10E
#define U_STATE        0x110
#define U_ALIVE        0x10000000u
#define U_TYPE         0x92         /* the type row: its ID says whether a slot changed type      */
#define ORD_VTABLE     0x4FD2C8u    /* every live node's (0x43A0DA, 0x43A131)                      */
#define ORD_UNIT       0x0E
#define ORD_FLAGS      0x42
#define ORD_SUBLIST    0x40000u     /* the node belongs on the sub list (the linkers)              */
#define ORD_NEXT       0x4A

#define VERIFY_ROUND   30           /* iterations in which every unit slot is checked once        */
#define VERIFY_TABLE   16384        /* bytes of declared rows checked each iteration, at least a row */
#define VERIFY_CHUNK   0x7000       /* the most bytes of declared rows one verify record carries  */
#define SPAN_MAX       0x7000       /* the most image bytes one RK_TABLE_SPAN carries; a declared
                                       row wider than this is refused, since a row goes whole  */
#define SPAN_GAP       8            /* unchanged bytes a span bridges rather than splitting        */
#define HEADER_MAX     0xFF00       /* an RK_HEADER's body; its length is a u16                    */
#define IMG_DWORDS     (REC_PLAYER_DWORDS > REC_UNIT_DWORDS ? REC_PLAYER_DWORDS : REC_UNIT_DWORDS)
#define ORD_SCRATCH    (REC_ORDERS_MAX * REC_ORDER_IMAGE)

/* ---- the layout, as masks ------------------------------------------------------------------- */

/* Per image dword, the bits that are values or id bytes; per id row, where it is in the image and
   its table. Four images: a unit's (its record, then its mover's), a player's (its record, then
   its PlayerInfo's), an order node's and a projectile's. Built once from the generated table, which
   the build keeps equal to replay-layout.tsv. */
#define RL_ROWS (sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0])
typedef struct { unsigned short off; unsigned char table; } IdRow;
/* the feature images' value bits: a cell's, and a wreck record's as each kind of def lays it out
   (*The features*) */
static unsigned char s_cmask[REC_CELL_BYTES];
static unsigned char s_w3mask[REC_WRECK_BYTES], s_wgmask[REC_WRECK_BYTES];
static void feat_masks_row(const ReplayLayoutRow* r);
enum { IMG_UNIT, IMG_PLAYER, IMG_ORDER, IMG_PROJ, IMG_KINDS };
static unsigned s_umask[REC_UNIT_DWORDS], s_pmask[REC_PLAYER_DWORDS];
static unsigned s_omask[REC_ORDER_IMAGE / 4], s_jmask[REC_PROJ_DWORDS];
static IdRow    s_ids[IMG_KINDS][RL_ROWS];           /* an image's id rows are at most every row */
static unsigned s_nids[IMG_KINDS], s_masked;

static void masks_init(void)
{
    static unsigned char b[IMG_KINDS][REC_PLAYER_IMAGE];
    unsigned i, k;
    if (s_masked) return;
    for (k = 0; k < IMG_KINDS; k++) memset(b[k], 0, sizeof b[k]);
    memset(b[IMG_UNIT], 0xFF, REC_MOVER_AT + REC_MOVER_BYTES);      /* the pads stay zero */
    memset(b[IMG_PLAYER], 0xFF, REC_PLAYER_BYTES);
    memset(b[IMG_PLAYER] + REC_INFO_AT, 0xFF, REC_INFO_BYTES);
    memset(b[IMG_ORDER], 0xFF, REC_ORDER_BYTES);
    memset(b[IMG_PROJ], 0xFF, REC_PROJ_BYTES);
    memset(s_cmask, 0xFF, sizeof s_cmask);
    memset(s_w3mask, 0xFF, sizeof s_w3mask);
    memset(s_wgmask, 0xFF, sizeof s_wgmask);
    for (i = 0; i < RL_ROWS; i++) {
        const ReplayLayoutRow* r = &tagpu_replay_layout[i];
        unsigned img = r->record == RL_PLAYER || r->record == RL_INFO ? IMG_PLAYER :
                       r->record == RL_ORDER ? IMG_ORDER : r->record == RL_PROJ ? IMG_PROJ :
                       IMG_UNIT;
        if (r->record == RL_CELL || r->record == RL_WRECK3DO || r->record == RL_WRECKGAF) {
            feat_masks_row(r);
            continue;
        }
        unsigned at = r->offset + (r->record == RL_MOVER ? REC_MOVER_AT :
                                   r->record == RL_INFO ? REC_INFO_AT : 0);
        if (r->target != RL_T_NONE) {
            s_ids[img][s_nids[img]].off = (unsigned short)at;
            s_ids[img][s_nids[img]].table = r->target;
            s_nids[img]++;
            continue;
        }
        if (r->cls == RL_PUPPET0 || r->cls == RL_PUPPET1) continue;   /* values */
        for (k = 0; k < r->size; k++)
            b[img][at + k] &= (unsigned char)(r->mask ? ~(r->mask >> (8 * k)) : 0);
    }
    memcpy(s_umask, b[IMG_UNIT], sizeof s_umask);
    memcpy(s_pmask, b[IMG_PLAYER], sizeof s_pmask);
    memcpy(s_omask, b[IMG_ORDER], sizeof s_omask);
    memcpy(s_jmask, b[IMG_PROJ], sizeof s_jmask);
    s_masked = 1;
}

/* ---- the level and its IDs ------------------------------------------------------------------ */

typedef struct Level {
    char*    ta;
    char*    begin;         /* the unit array                                    */
    unsigned nslots;
    char*    defs;          /* the unit-type definitions and how many             */
    unsigned ndefs;
    char*    buckets;       /* the sort buckets and how many                      */
    unsigned nbuckets;
    char*    offmap;
    char*    proj;          /* the projectile pool, TAGPU_LIM_PROJ records        */
} Level;

static char* ta_main(void)
{
    return *(char**)(size_t)TA_MAINPP;
}

static int game_mode(const char* ta)
{
    const char* gs = *(char* const*)(ta + OFF_GSTATE);
    return gs ? *(const int*)gs : 0;
}

static int level_read(char* ta, Level* L)
{
    unsigned short n = *(unsigned short*)(ta + OFF_NSLOTS);
    char* b = *(char**)(ta + OFF_BEGIN);
    char* last = *(char**)(ta + OFF_LAST);
    unsigned cols = *(unsigned*)(ta + OFF_BCOLS), rows = *(unsigned*)(ta + OFF_BROWS);
    if (!b || n < 2 || last != b + (size_t)(n - 1) * REC_UNIT_BYTES) return 0;
    L->ta = ta;
    L->begin = b;
    L->nslots = n;
    L->defs = *(char**)(ta + OFF_UDEFS);
    L->ndefs = *(unsigned*)(ta + OFF_UCOUNT);
    L->buckets = *(char**)(ta + OFF_BUCKETS);
    L->nbuckets = (cols && rows && cols < 0x10000 && rows < 0x10000) ? cols * rows : 0;
    L->offmap = *(char**)(ta + OFF_OFFMAP);
    L->proj = *(char**)(ta + OFF_PROJ);
    return 1;
}

static unsigned s_bad;                   /* pointers outside their tables, this recording      */
static unsigned s_badlog;                /* of them, the ones logged                           */

/* a row's ID: one more than its index, so NULL's ID is 0 (tagpu_recfmt.h) */
static unsigned id_in(const char* p, const char* base, unsigned stride, unsigned n)
{
    size_t off;
    if (!base || p < base) return REC_ID_BAD;
    off = (size_t)(p - base);
    if (off % stride || off / stride >= n) return REC_ID_BAD;
    return (unsigned)(off / stride) + 1;
}

/* The ID of `p` in the table the exclusion file names; never a dereference. */
static unsigned id_of(const Level* L, unsigned table, const char* p)
{
    unsigned id;
    int w;
    if (!p) return REC_ID_NULL;
    switch (table) {
    case RL_T_UNIT:   id = id_in(p, L->begin, REC_UNIT_BYTES, L->nslots); break;
    case RL_T_TYPE:   id = id_in(p, L->defs, UD_STRIDE, L->ndefs); break;
    case RL_T_PLAYER: id = id_in(p, L->ta + OFF_PLAYERS, PL_STRIDE, PL_ALL); break;
    case RL_T_WEAPON:
        w = tagpu_limits_weapon_index(p);
        id = w < 0 ? REC_ID_BAD : (unsigned)w + 1;
        break;
    case RL_T_BUCKET:
        /* one past the last bucket included: stock forms it for a footprint 0 wide at the east
           edge (the engine map, *The sort bucket is taken from the position*) */
        if (p == L->offmap) return REC_ID_OFFMAP;
        id = id_in(p, L->buckets, BUCKET_STRIDE, L->nbuckets + 1);
        break;
    case RL_T_PROJ:
        /* a record of the pool, which the allocation 0x499A30 sizes to TAGPU_LIM_PROJ records
           (tagpu_limits.h: the raise is installed whole or the process ends) */
        id = id_in(p, L->proj, REC_PROJ_BYTES, TAGPU_LIM_PROJ);
        break;
    case RL_T_OWNED:
        /* the one id row read through its pointer: an order node's own object, live while the
           node holds it (the head of this file); its class code by its vtable, the restore's
           table 0x43A918 */
        switch (*(const unsigned*)p) {
        case 0x4FD3B8u: id = 2; break;
        case 0x4FD3F8u: id = 3; break;
        case 0x4FD328u: id = 4; break;
        case 0x4FD358u: id = 5; break;
        case 0x4FD388u: id = 6; break;
        default:        id = REC_ID_BAD; break;
        }
        break;
    default:          id = REC_ID_BAD; break;
    }
    if (id == REC_ID_BAD) s_bad++;
    return id;
}

static unsigned id_of_sim(const Level* L, unsigned simid, const char* p)
{
    if (!p) return REC_ID_NULL;
    switch (simid) {
    case SIM_ID_UNIT_SLOT: return id_of(L, RL_T_UNIT, p);
    case SIM_ID_UNIT_TYPE: return id_of(L, RL_T_TYPE, p);
    case SIM_ID_PLAYER:    return id_of(L, RL_T_PLAYER, p);
    case SIM_ID_WEAPON:    return id_of(L, RL_T_WEAPON, p);
    default:               s_bad++; return REC_ID_BAD;
    }
}

static void bad_log(const char* what, unsigned off, const void* p)
{
    if (s_badlog >= 8) return;
    s_badlog++;
    tagpu_logf("recorder: %s +0x%X holds %p, outside its table", what, off, p);
}

/* ---- images --------------------------------------------------------------------------------- */

/* An image from a record's bytes as read, `raw`: its values, and each id row's ID in place of
   the pointer there. */
static void image(const Level* L, const unsigned char* raw, unsigned dwords, const unsigned* mask,
                  unsigned kind, unsigned* img)
{
    static const char* const what[IMG_KINDS] = { "a unit's", "a player's", "an order node's",
                                                 "a projectile's" };
    unsigned i;
    const char* p;
    memcpy(img, raw, (size_t)dwords * 4);
    for (i = 0; i < dwords; i++) img[i] &= mask[i];
    for (i = 0; i < s_nids[kind]; i++) {
        unsigned id;
        memcpy(&p, raw + s_ids[kind][i].off, sizeof p);
        id = id_of(L, s_ids[kind][i].table, p);
        if (id == REC_ID_BAD) bad_log(what[kind], s_ids[kind][i].off, p);
        memcpy((unsigned char*)img + s_ids[kind][i].off, &id, 4);
    }
}

static const unsigned char s_zero[SPAN_MAX];   /* a row of zeros, the widest a table may have */

/* the first byte at or after `i` where `a` and `b` differ, or `n` */
static unsigned diff_from(const unsigned char* a, const unsigned char* b, unsigned i, unsigned n)
{
    while (i + 16 <= n) {
        uint32_t x[4], y[4];
        memcpy(x, a + i, 16);
        memcpy(y, b + i, 16);
        if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) break;
        i += 16;
    }
    while (i < n && a[i] == b[i]) i++;
    return i;
}

/* A unit's bytes as its image takes them: its record, then its mover's 0x2F bytes, or zeros when
   it has none (the head of this file says why the read is sound). */
static void unit_raw(const Level* L, unsigned slot, unsigned char* raw)
{
    const unsigned char* rec = (const unsigned char*)L->begin + (size_t)slot * REC_UNIT_BYTES;
    const unsigned char* mv = *(const unsigned char* const*)(rec + U_MOVER);
    memcpy(raw, rec, REC_UNIT_BYTES);
    memset(raw + REC_MOVER_AT, 0, REC_UNIT_IMAGE - REC_MOVER_AT);
    if (mv) memcpy(raw + REC_MOVER_AT, mv, REC_MOVER_BYTES);
}

/* the mover row's dword says whether the tail is a mover's */
static void unit_image_raw(const Level* L, const unsigned char* raw, unsigned* img)
{
    image(L, raw, REC_UNIT_DWORDS, s_umask, IMG_UNIT, img);
    if (*(const unsigned*)(raw + U_MOVER)) img[U_MOVER / 4] = 1;
}

static void unit_image(const Level* L, unsigned slot, unsigned* img)
{
    unsigned char raw[REC_UNIT_IMAGE];
    unit_raw(L, slot, raw);
    unit_image_raw(L, raw, img);
}

/* A player's image: its record, the pad byte, then its PlayerInfo's 0xB9 bytes; the info row's
   dword holds 1 when the record has one. */
static void player_image(const Level* L, unsigned i, unsigned* img)
{
    unsigned char raw[REC_PLAYER_IMAGE];
    const unsigned char* rec = (const unsigned char*)L->ta + OFF_PLAYERS + (size_t)i * PL_STRIDE;
    const unsigned char* info = *(const unsigned char* const*)(rec + PL_INFO);
    unsigned one = 1;
    memset(raw, 0, sizeof raw);
    memcpy(raw, rec, REC_PLAYER_BYTES);
    if (info) memcpy(raw + REC_INFO_AT, info, REC_INFO_BYTES);
    image(L, raw, REC_PLAYER_DWORDS, s_pmask, IMG_PLAYER, img);
    if (info) memcpy((unsigned char*)img + PL_INFO, &one, 4);
}

static int unit_live(const Level* L, unsigned slot)
{
    unsigned st;
    memcpy(&st, L->begin + (size_t)slot * REC_UNIT_BYTES + U_STATE, 4);
    return (st & U_ALIVE) != 0;
}

/* A declared table: where its rows are and how many bytes of them are live now, by its own
   declaration -- a static array's rows, a live prefix clamped to them, a heap table's count. */
static const unsigned char* table_rows(const SimDesc* d, unsigned* bytes)
{
    const unsigned char* base = (d->flags & SIMD_HEAP) ? *(unsigned char* const*)d->addr
                                                       : (const unsigned char*)d->addr;
    unsigned rows = d->nrows;
    if (d->rows) rows = !d->nrows ? *d->rows : (*d->rows < d->nrows ? *d->rows : d->nrows);
    *bytes = base ? rows * d->stride : 0;
    return base;
}

/* One row's image: its values, the IDs of the addresses its layout names, zero for the rest. Every
   field lies inside the row: the registry's attach walk refuses a declaration with a field past its
   row's end, and only the declarations it accepted reach the recorder. A row of zeros images to
   zeros, its addresses being NULL, whose ID is 0 (tagpu_recfmt.h): row_emit passes such a row
   over without imaging it, and the rotating check images every row it checks. */
static void row_image(const Level* L, const SimDesc* d, const unsigned char* row,
                      unsigned char* img)
{
    const SimField* f;
    memcpy(img, row, d->stride);
    for (f = d->layout; f && f->size; f++) {
        unsigned k, n = f->repeat ? f->repeat : 1;
        for (k = 0; k < n; k++) {
            unsigned char* at = img + f->off + (size_t)k * f->step;
            switch (f->kind) {
            case SIMF_ADDR:
                if (f->id != SIM_ID_NONE && f->size == sizeof(char*)) {
                    const char* p;
                    unsigned id;
                    memcpy(&p, at, sizeof p);
                    id = id_of_sim(L, f->id, p);
                    if (id == REC_ID_BAD) bad_log(d->name, (unsigned)(at - img), p);
                    memcpy(at, &id, 4);
                } else {
                    memset(at, 0, f->size);
                }
                break;
            case SIMF_LOCAL: case SIMF_OWNED: case SIMF_COLUMN:
                memset(at, 0, f->size);
                break;
            default:
                break;
            }
        }
    }
}

/* The tables a recording carries: tagpu_simreg_recorded, the test the replay pairs by too */
static int table_recorded(const SimDesc* d)
{
    return tagpu_simreg_recorded(d);
}

/* 1 when a declaration's layout zeroes every byte of its row, as row_image does, so its image is
   zero whatever the row holds and there is nothing to compare */
static int image_zero(const SimDesc* d)
{
    static unsigned char z[SPAN_MAX];
    const SimField* f;
    unsigned i;
    if (!d->layout || d->stride > SPAN_MAX) return 0;
    memset(z, 0, d->stride);
    for (f = d->layout; f->size; f++) {
        unsigned k, n = f->repeat ? f->repeat : 1;
        if (!(f->kind == SIMF_LOCAL || f->kind == SIMF_OWNED || f->kind == SIMF_COLUMN ||
              (f->kind == SIMF_ADDR && (f->id == SIM_ID_NONE || f->size != sizeof(char*)))))
            continue;
        for (k = 0; k < n; k++) memset(z + f->off + (size_t)k * f->step, 1, f->size);
    }
    for (i = 0; i < d->stride; i++)
        if (!z[i]) return 0;
    return 1;
}

/* the first recorded table whose row is wider than a record carries, or NULL */
static const SimDesc* table_too_wide(void)
{
    unsigned i;
    for (i = 0; i < tagpu_simreg_count(); i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        if (d && table_recorded(d) && d->stride > SPAN_MAX) return d;
    }
    return NULL;
}

/* ---- one recording -------------------------------------------------------------------------- */

typedef struct Table {
    const SimDesc* d;
    unsigned       index;       /* in the registry's list: what the records call it     */
    unsigned       cap, size;   /* bytes of room taken at the start, and of image now   */
    unsigned char* shadow;      /* the image as the ring last carried it                */
    unsigned char* raw;         /* a static table's bytes as last read, when its image is
                                   not its bytes (a layout); else the shadow serves      */
    const unsigned char* base;  /* where its rows were at the last capture              */
    unsigned       limit;       /* a heap table's allocation, from `base` (heap_limit)  */
    PVOID*         pages;       /* a heap table's written pages (GetWriteWatch)         */
    ULONG_PTR      npages;      /* their room                                           */
    int            unwatched;   /* GetWriteWatch refused it: diffed whole each time     */
    int            overcount;   /* its count has said more than its allocation holds    */
    int            zero;        /* its layout zeroes every byte: its image is all zero  */
    unsigned       lhash;       /* tagpu_simreg_layout_hash, for the header             */
} Table;

static int table_grow(Table* tb, unsigned bytes);

typedef struct Feat Feat;          /* *The features* */

/* An order list's image as the ring last carried it: its nodes' images, in order. */
typedef struct OList { unsigned n, cap; unsigned char img[]; } OList;

typedef struct Rec {
    RecRing*       ring;
    HANDLE         wake;        /* the capture's own duplicates of the ring's events     */
    HANDLE         done;
    int            fin;         /* the slot of s_fin its end hands `done` to            */
    unsigned       signalled;   /* w at the last signal                                 */
    unsigned       w;           /* bytes written, published or not                      */
    unsigned       high;        /* the most bytes the ring has held                     */
    char*          begin;       /* the level it records                                 */
    unsigned       nslots;
    int            tick;        /* the last GameTime captured                           */
    int            first;       /* the first iteration is still to be captured          */
    Level          L;           /* the level as the last capture read it: the events'   */
    DWORD          tid;         /* the game thread, the capture's                       */
    unsigned char* ushadow;     /* nslots images                                        */
    unsigned char* uraw;        /* nslots records and movers as the last capture read them */
    unsigned char* ulive;       /* nslots flags                                         */
    unsigned       pshadow[REC_PLAYERS][REC_PLAYER_DWORDS];
    unsigned char  ctl[REC_PLAYERS];             /* the controllers and the two players */
    unsigned char  view[2];                      /* as the ring last carried them        */
    HANDLE         oheap;       /* the order lists' images: this thread's alone           */
    OList**        olist;       /* nslots * 2, main then sub; NULL is an empty list     */
    unsigned char* oscratch;    /* one list's images as read now                        */
    unsigned       oodd;        /* nodes whose owner or list flag disagrees with the list holding them */
    unsigned       uncredited;  /* nano sprays no query preceded in their handler's call    */
    int            ev_full;     /* an event did not fit: the step's events are incomplete */
    Table*         tab;
    unsigned       ntab;
    unsigned char* rowbuf;      /* one row's image, the widest recorded stride          */
    unsigned       vslot, vplayer, vtab, voff;   /* where the rotating check is         */
    unsigned       iterations;                   /* captured, and their cost in QPC ticks: */
    LONGLONG       cost, cost_max, cost_first;   /* the first images everything             */
    int            max_tick;                     /* the costliest one's GameTime and bytes */
    unsigned       max_bytes;
    unsigned       pages, pages_now, pages_max;  /* heap-table pages written, as GetWriteWatch
                                                    counts them: after the first iteration,
                                                    in this one and in one at most         */
    Feat*          feat;        /* the grid's and the pool's copies (*The features*)      */
    unsigned       gshadow[REC_GLOBALS_DWORDS];  /* the globals image as the ring last carried it */
    const unsigned char* jpool; /* the projectile pool the recording began with (*The globals
                                   and the projectiles*)                                   */
    unsigned char* jraw;        /* its records' bytes as last imaged, TAGPU_LIM_PROJ of them */
    unsigned char* jshadow;     /* their images as the ring last carried them             */
    unsigned       jcount, jhw; /* the count and the written extent the ring last carried  */
    unsigned       vproj;       /* the next record the rotating check images              */
    unsigned       jimages;     /* records the stream carried, whole or as a delta        */
    const unsigned char* mmap;  /* the explored map the recording began with (*The explored
                                   map*), mbytes of it                                     */
    unsigned       mbytes;
    unsigned char* mshadow;     /* its bytes as the ring last carried them                */
    unsigned       mruns;       /* RK_MAPPED_RUNS the stream carried                       */
    unsigned long long mrun_bytes;   /* and the map's bytes in them                        */
    unsigned char  mrunbuf[REC_MAPPED_RUNS];    /* an RK_MAPPED_RUNS as it is built          */
    unsigned short seated;              /* bit i: the header seats record i (RT_PLAYER)          */
    unsigned       cmdr[REC_PLAYERS];   /* the GameTime each seat's last commander died, 0 none  */
    unsigned       defeat[REC_PLAYERS]; /* the GameTime its live count last reached 0, 0 none:
                                           a seat ends defeated only while it is still 0       */
} Rec;

static void olist_drop(Rec* r, unsigned slot);
static void feat_free(Feat* f);
static Feat* feat_start(const char* ta, RecRing* g);
static int cap_features(Rec* r, const Level* L);
static unsigned fdefs_hash(const char* ta, unsigned n);
static unsigned feat_unobserved(const Rec* r);
static void feat_log(const Rec* r);
static void dump_rec(unsigned kind, const void* pre, unsigned npre, const void* pay, unsigned npay);
static unsigned mapped_bytes(const char* ta);

enum { ST_IDLE, ST_RECORDING, ST_DONE };        /* DONE: the rest of this level is not recorded */
static Rec*     s_rec;
static int      s_state = ST_IDLE;
static RecFork  s_fork;         /* a fork armed for the next capture point; file NULL for none */

/* THE GAME THREAD'S RECORDING. s_tid is the recording's game thread while s_rec is set, and 0
   otherwise; only that thread writes either. An event hook compares s_tid with its own thread
   before it touches s_rec, so a call on another thread never reads the recording -- it counts
   itself in s_offthread and records nothing. */
static volatile DWORD s_tid;
static volatile LONG  s_offthread;               /* this recording's refused calls          */
/* 1 while a skirmish is recorded: the five fire functions then build their 0x0D (*The events*) */
static volatile unsigned char s_rec_fired;

/* ---- the ring ------------------------------------------------------------------------------- */

static unsigned ring_used(const Rec* r)
{
    return r->w - (unsigned)InterlockedCompareExchange(&r->ring->rpub, 0, 0);
}

/* Room for a record of `body` bytes at the write position, an RK_PAD first when it would cross
   the ring's end; returns where its header goes, or NULL when it would eat into the reserve. */
static unsigned char* ring_take(Rec* r, unsigned body)
{
    unsigned size = (4 + body + 3) & ~3u, pos = r->w & (REC_RING_BYTES - 1), pad = 0;
    if (pos + size > REC_RING_BYTES) pad = REC_RING_BYTES - pos;
    if (ring_used(r) + pad + size + REC_RING_RESERVE > REC_RING_BYTES) return NULL;
    if (pad) {
        RecHdr h = { RK_PAD, 0 };
        memcpy(r->ring->buf + pos, &h, 4);
        r->w += pad;
        pos = 0;
    }
    return r->ring->buf + pos;
}

static void ring_commit(Rec* r, unsigned char* at, unsigned kind, unsigned body)
{
    RecHdr h;
    unsigned used;
    h.kind = (uint16_t)kind;
    h.len = (uint16_t)body;
    memcpy(at, &h, 4);
    r->w += (4 + body + 3) & ~3u;
    used = ring_used(r);
    if (used > r->high) r->high = used;
}

/* a record of a fixed prefix and a payload; 0 when it does not fit */
static int ring_put(Rec* r, unsigned kind, const void* pre, unsigned npre, const void* pay,
                    unsigned npay)
{
    unsigned char* at = ring_take(r, npre + npay);
    if (!at) return 0;
    if (npre) memcpy(at + 4, pre, npre);
    if (npay) memcpy(at + 4 + npre, pay, npay);
    ring_commit(r, at, kind, npre + npay);
    return 1;
}

/* Publishes what the capture has written; signals the writer when REC_WAKE_BYTES have gone out
   since the last signal (tagpu_recorder.h, *Waking the writer*), or when `now`. */
static void ring_publish(Rec* r, int now)
{
    InterlockedExchange(&r->ring->wpub, (LONG)r->w);
    if (now || r->w - r->signalled >= REC_WAKE_BYTES) {
        SetEvent(r->wake);
        r->signalled = r->w;
    }
}

/* ---- the header ----------------------------------------------------------------------------- */

typedef struct Tlv { unsigned char* b; unsigned n, cap; int full; } Tlv;

static void tlv(Tlv* t, unsigned tag, const void* p, unsigned n)
{
    unsigned size = (4 + n + 3) & ~3u;
    unsigned short h[2];
    if (t->full || t->n + size > t->cap || n > 0xFFFF) { t->full = 1; return; }
    h[0] = (unsigned short)tag;
    h[1] = (unsigned short)n;
    memcpy(t->b + t->n, h, 4);
    if (n) memcpy(t->b + t->n + 4, p, n);
    memset(t->b + t->n + 4 + n, 0, size - 4 - n);
    t->n += size;
}

static void tlv_u32(Tlv* t, unsigned tag, unsigned v) { tlv(t, tag, &v, 4); }

static size_t bounded_len(const char* s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

/* The map's stem, from GameingState's TNT path: "Maps\Two Continents.TNT" -> "Two Continents".
   The path is a buffer inside the object (its fields run past +0xDBC), read to MAX_PATH bytes. */
static void map_stem(const char* ta, char* out, size_t cap)
{
    const char* gs = *(char* const*)(ta + OFF_GSTATE);
    const char* s = gs + GS_TNT;
    const char* q;
    size_t n = 0;
    if (!gs) { snprintf(out, cap, "map"); return; }
    for (q = s; q < gs + GS_TNT + MAX_PATH && *q; q++)
        if (*q == '\\' || *q == '/' || *q == ':') s = q + 1;
    for (q = s; q < gs + GS_TNT + MAX_PATH && *q && *q != '.' && n + 1 < cap; q++)
        out[n++] = (*q >= 0x20 && *q < 0x7F && !strchr("<>\"|?*", *q)) ? *q : '_';
    out[n] = 0;
    if (!n) snprintf(out, cap, "map");
}

/* FNV-1a over names of at most `len` bytes, `n` of them `stride` apart; a zero byte ends each */
static unsigned names_hash(const char* base, unsigned n, unsigned stride, unsigned len)
{
    unsigned i, h = 2166136261u;
    for (i = 0; i < n; i++) {
        const char* name = base + (size_t)i * stride;
        size_t k, m = bounded_len(name, len);
        for (k = 0; k < m; k++) h = (h ^ (unsigned char)name[k]) * 16777619u;
        h *= 16777619u;
    }
    return h;
}

void tagpu_rec_map_stem(const char* ta, char* out, size_t cap) { map_stem(ta, out, cap); }

void tagpu_rec_defs_print(const char* ta, unsigned* n, unsigned* hash)
{
    const char* defs = *(const char* const*)(ta + OFF_UDEFS);
    *n = *(const unsigned*)(ta + OFF_UCOUNT);
    *hash = defs ? names_hash(defs + UD_NAME, *n, UD_STRIDE, UD_NAMELEN) : 0;
}

void tagpu_rec_weapons_print(unsigned* n, unsigned* hash)
{
    const char* w0 = tagpu_limits_weapon0();
    *n = w0 ? (unsigned)tagpu_limits_weapon_slots() : 0;
    *hash = w0 ? names_hash(w0, *n, WPN_STRIDE, WPN_NAMELEN) : 0;
}

void tagpu_rec_features_print(const char* ta, unsigned* w, unsigned* h, unsigned* ndefs,
                              unsigned* hash)
{
    *w = *(const unsigned*)(ta + 0x14233);
    *h = *(const unsigned*)(ta + 0x14237);
    *ndefs = *(const unsigned*)(ta + 0x14253);
    *hash = fdefs_hash(ta, *ndefs);
}

/* THE MOD. The engine's mounted archives are the list at [0x51FBD0]+0x618 (handles), +0x61C its
   count, each handle's +0x14 the archive's full path in a buffer of 0x104 bytes (OpenHAPIFile
   0x4BDD70). Only InitTAHPIAry 0x41D4C0 mounts, prunes or reallocates the list, from its eleven
   callers on the game thread (the skirmish's Start last, 0x47AF2E); this reads it on the game
   thread too, so the list cannot move under it. BOUND: the engine's count, which the allocation
   always holds (the mount reallocates to count+1, 0x4BE140), capped at ARC_SANE to bound the time;
   a path is read within its buffer. */
#define VERSION_TEXT 0x005031B4u    /* the main menu's version (0x4263B0 puts it into DebugString):
                                       "v3.1", or what a mod's patch wrote there; 7 characters fit */
#define GFX_PP       0x0051FBD0u    /* the graphics globals (0x4B6220 returns it)           */
#define ARC_LIST     0x618
#define ARC_COUNT    0x61C
#define ARC_PATH     0x14
#define ARC_PATHMAX  0x104
#define ARC_SANE     4096u
#define IMPURE_UFO   "impure-patch.ufo"   /* Impure's own archive: not the mod (tagpu_menu.c) */

typedef void (*ArcVisit)(void* ctx, const char* name, unsigned n, unsigned size, const FILETIME* t);

static unsigned fnv(unsigned h, const void* p, unsigned n)
{
    const unsigned char* b = (const unsigned char*)p;
    while (n--) h = (h ^ *b++) * 16777619u;
    return h;
}

/* Each mounted archive in mount order, its name without its folder, size and mtime (0 when the
   file cannot be asked), to `visit` for the first REC_ARCHIVES; the fingerprint of all but
   Impure's own, over their names and sizes: two installs of one mod agree whatever their files'
   times. `*n` the archives the engine holds. */
static unsigned archives_each(ArcVisit visit, void* ctx, unsigned* total)
{
    const char* gfx = *(const char* const*)GFX_PP;
    const char* const* list;
    unsigned count, i, h = 2166136261u;
    *total = 0;
    if (!gfx) return h;
    list = *(const char* const* const*)(gfx + ARC_LIST);
    count = *(const unsigned*)(gfx + ARC_COUNT);
    if (!list) return h;
    *total = count;
    if (count > ARC_SANE) count = ARC_SANE;
    for (i = 0; i < count; i++) {
        const char* path = list[i] ? list[i] + ARC_PATH : NULL;
        const char* name;
        char low[ARC_PATHMAX];
        unsigned n, k, size = 0;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        FILETIME mt = { 0, 0 };
        if (!path) continue;
        n = (unsigned)bounded_len(path, ARC_PATHMAX);
        if (n == ARC_PATHMAX) continue;           /* a buffer the mount left unwritten */
        name = path + n;
        while (name > path && name[-1] != '\\' && name[-1] != '/') name--;
        n = (unsigned)(path + n - name);
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
            size = fa.nFileSizeLow;
            mt = fa.ftLastWriteTime;
        }
        if (visit && i < REC_ARCHIVES) visit(ctx, name, n, size, &mt);
        for (k = 0; k < n; k++)
            low[k] = (char)(name[k] >= 'A' && name[k] <= 'Z' ? name[k] + 32 : name[k]);
        if (n == sizeof IMPURE_UFO - 1 && !memcmp(low, IMPURE_UFO, n)) continue;
        low[n] = 0;
        h = fnv(h, low, n + 1);
        h = fnv(h, &size, 4);
    }
    return h;
}

unsigned tagpu_rec_mod_print(void)
{
    unsigned n;
    return archives_each(NULL, NULL, &n);
}

static void archive_entry(void* ctx, const char* name, unsigned n, unsigned size, const FILETIME* t)
{
    unsigned char b[12 + ARC_PATHMAX];
    memcpy(b, &size, 4);
    memcpy(b + 4, t, 8);
    memcpy(b + 12, name, n);
    tlv((Tlv*)ctx, RT_ARCHIVE, b, 12 + n);
}

/* the header's entries; `fork`, for a fork's, adds RT_TIMELINE after RT_START */
static void header_build(const Level* L, int tick, const char* map, const FILETIME* now,
                         const RecFork* fork, Tlv* t)
{
    unsigned i, n;
    unsigned char pl[16 + REC_INFO_BYTES + PL_NAMELEN];
    const char* ta = L->ta;
    const char* set = *(const char* const*)(ta + OFF_SETTINGS);

    tlv_u32(t, RT_FORMAT, REC_FORMAT);
    tlv(t, RT_BUILD, GIT_COMMIT, (unsigned)strlen(GIT_COMMIT));
    tlv_u32(t, RT_MODE, MODE_SKIRMISH);
    tlv(t, RT_MAP, map, (unsigned)strlen(map));
    tlv_u32(t, RT_START, (unsigned)tick);
    if (fork) {
        unsigned d[3];
        d[0] = fork->id;
        d[1] = fork->parent;
        d[2] = fork->fork_at;
        tlv(t, RT_TIMELINE, d, 12);
    }
    tlv(t, RT_WALLCLOCK, now, 8);
    tlv_u32(t, RT_SLOTS, L->nslots);
    tlv_u32(t, RT_UNITS_EACH, *(const unsigned short*)(ta + OFF_EACH));
    {
        unsigned d[2];
        tagpu_rec_defs_print(ta, &d[0], &d[1]);
        tlv(t, RT_UNITDEFS, d, 8);
    }
    for (i = 0; i < REC_PLAYERS; i++) {
        const char* rec = ta + OFF_PLAYERS + (size_t)i * PL_STRIDE;
        const unsigned char* info = *(const unsigned char* const*)(rec + PL_INFO);
        if (!rec[PL_CONTROLLER]) continue;
        pl[0] = (unsigned char)i;
        pl[1] = (unsigned char)rec[PL_CONTROLLER];
        pl[2] = info ? info[0x95] : 0xFF;
        pl[3] = info ? info[0x96] : 0xFF;
        memcpy(pl + 4, rec + PL_ALLY, 11);
        pl[15] = 0;
        if (info) memcpy(pl + 16, info, REC_INFO_BYTES);
        else memset(pl + 16, 0xFF, REC_INFO_BYTES);
        n = (unsigned)bounded_len(rec + PL_NAME, PL_NAMELEN);
        memcpy(pl + 16 + REC_INFO_BYTES, rec + PL_NAME, n);
        tlv(t, RT_PLAYER, pl, 16 + REC_INFO_BYTES + n);
    }
    for (i = 0; i < tagpu_simreg_levers(); i++) {
        int set_, len;
        const char* text;
        const char* file = tagpu_simreg_lever_at(i, &set_, &text, &len);
        unsigned char b[4 + 64 + 256];
        unsigned nf = (unsigned)bounded_len(file, 64), nt = text ? (unsigned)strlen(text) : 0;
        if (nt > 256) nt = 256;
        b[0] = (unsigned char)set_;
        b[1] = (unsigned char)(text != NULL);
        b[2] = (unsigned char)nf;
        b[3] = 0;
        memcpy(b + 4, file, nf);
        if (nt) memcpy(b + 4 + nf, text, nt);
        tlv(t, RT_LEVER, b, 4 + nf + nt);
    }
    tlv(t, RT_LAYOUT, TAGPU_REPLAY_LAYOUT_SHA256, (unsigned)strlen(TAGPU_REPLAY_LAYOUT_SHA256));
    /* the settings are the SKIRMISH screen's: the loader has copied the options out of them
       (0x4973C1..0x497457) and reads the start mode from them (0x49790E) */
    {
        unsigned char o[20];
        unsigned v;
        unsigned short np = *(const unsigned short*)(ta + OFF_NPLAYERS);
        v = *(const unsigned short*)(ta + OFF_LOSTYPE) & 7u;   memcpy(o, &v, 4);
        v = *(const unsigned*)(ta + OFF_CMDRDEATH);            memcpy(o + 4, &v, 4);
        v = *(const unsigned*)(ta + OFF_DIFFICULTY);           memcpy(o + 8, &v, 4);
        v = set ? *(const unsigned*)(set + 0x118) : 0;         memcpy(o + 12, &v, 4);
        memcpy(o + 16, &np, 2);
        o[18] = (unsigned char)ta[OFF_CONTROLLED];
        o[19] = (unsigned char)ta[OFF_VIEWED];
        tlv(t, RT_OPTIONS, o, 20);
    }
    if (set) tlv(t, RT_SETTINGS_MAP, set + 0x11C, (unsigned)bounded_len(set + 0x11C, 64));
    {
        unsigned d[2];
        tagpu_rec_weapons_print(&d[0], &d[1]);
        tlv(t, RT_WEAPONS, d, 8);
    }
    {
        unsigned d[5];
        tagpu_rec_features_print(ta, &d[0], &d[1], &d[3], &d[4]);
        d[2] = TAGPU_LIM_WRECKS;
        tlv(t, RT_FEATURES, d, 20);
    }
    tlv(t, RT_GLOBALS, TAGPU_REPLAY_GLOBALS_SHA256, (unsigned)strlen(TAGPU_REPLAY_GLOBALS_SHA256));
    tlv_u32(t, RT_PROJECTILES, TAGPU_LIM_PROJ);
    {
        const char* v = (const char*)VERSION_TEXT;
        unsigned k = 0;
        while (k < 8 && v[k] >= 0x20 && v[k] < 0x7F) k++;
        if (k && k < 8 && !v[k]) tlv(t, RT_GAME_VERSION, v, k);
    }
    {
        unsigned d[2];
        d[0] = archives_each(archive_entry, t, &d[1]);
        tlv(t, RT_MODPRINT, d, 8);
    }
}

/* The TNT's minimap picture as an RB_MINIMAP's payload into `b`: its size, or 0 when the map has
   none or it does not fit a ring record. The frame (main+0x1426B) lives for the whole level: LoadMap
   stores it (0x483900) and only the teardown frees it (0x483DFE). Its plane is uncompressed, w*h
   bytes at [frame+0x10] = frame+0x18, in a block the engine sized w*h+0x18 from the same w and h
   (0x4B8DA0): a plane anywhere else is not that frame's, and is not read. */
#define OFF_MINIMAP  0x1426B
#define OFF_MM_BOX   0x142EB        /* i16 columns, i16 rows: the part [0, 2*c) x [0, 2*r) shown */
static unsigned minimap_build(const char* ta, unsigned char* b)
{
    const unsigned char* g = *(const unsigned char* const*)(ta + OFF_MINIMAP);
    unsigned short w, h, box[2];
    const unsigned char* px;
    if (!g) return 0;
    memcpy(&w, g, 2);
    memcpy(&h, g + 2, 2);
    px = *(const unsigned char* const*)(g + 0x10);
    if (!w || !h || px != g + 0x18 || (unsigned)w * h > REC_MINIMAP_MAX) return 0;
    memcpy(box, ta + OFF_MM_BOX, 4);
    memcpy(b, &w, 2);
    memcpy(b + 2, &h, 2);
    memcpy(b + 4, box, 4);
    memcpy(b + REC_MINIMAP_HEAD, px, (size_t)w * h);
    return REC_MINIMAP_HEAD + (unsigned)w * h;
}

/* One RT_TABLE: the registry index the stream's table records carry, what the declaration is,
   and its layout's hash. The index is this build's registry order, which another build numbers
   otherwise, so a reader pairs two builds' tables by name. */
static void table_entry(Tlv* t, unsigned index, const SimDesc* d, unsigned lhash)
{
    unsigned char b[16 + 128];
    unsigned short ix = (unsigned short)index, fl = d->flags;
    unsigned n = (unsigned)bounded_len(d->name, 128);
    memcpy(b, &ix, 2);
    b[2] = d->cls;
    b[3] = d->key;
    memcpy(b + 4, &d->stride, 4);
    memcpy(b + 8, &fl, 2);
    b[10] = b[11] = 0;
    memcpy(b + 12, &lhash, 4);
    memcpy(b + 16, d->name, n);
    tlv(t, RT_TABLE, b, 16 + n);
}

static unsigned s_dllBase, s_dllSize;   /* Impure's DLL: its base and SizeOfImage (cnt_sinks_init) */

/* RT_FIELDS: a recorded table's layout, for `tarec scan`; nothing for a table without one */
static void table_fields(Tlv* t, unsigned index, const SimDesc* d)
{
    unsigned char b[4 + 12 * 64];
    unsigned short ix = (unsigned short)index, n = 0;
    const SimField* f;
    for (f = d->layout; f && f->size && n < 64; f++, n++) {
        unsigned char* e = b + 4 + 12 * n;
        memcpy(e, &f->off, 2);
        memcpy(e + 2, &f->size, 2);
        memcpy(e + 4, &f->repeat, 2);
        memcpy(e + 6, &f->step, 2);
        e[8] = f->kind;
        e[9] = f->id;
        e[10] = e[11] = 0;
    }
    if (!n) return;
    memcpy(b, &ix, 2);
    memcpy(b + 2, &n, 2);
    tlv(t, RT_FIELDS, b, 4 + 12u * n);
}

static void header_tables(const Rec* r, Tlv* t)
{
    unsigned i;
    for (i = 0; i < r->ntab; i++)
        table_entry(t, r->tab[i].index, r->tab[i].d, r->tab[i].lhash);
}

/* ---- the files still being finished --------------------------------------------------------- */

/* The `done` events of the recordings that have ended, which the exit waits for (tagpu_recorder.h,
   *The exit*). A recording takes a slot at its start and hands its event to it at its end, so an
   end always finds its slot: one recording is in progress at a time, and only its end fills a
   slot. The game thread takes and fills slots; tagpu_rec_exit may run on any thread, so the
   slots are read and written under s_fin_lock, which nothing holds across a wait. Once the exit
   has taken them, nothing closes their handles and no recording starts. */
#define REC_FINISHING 8
static HANDLE        s_fin[REC_FINISHING];
static volatile LONG s_fin_lock;
static int           s_fin_exiting;

static void fin_lock(void)
{
    while (InterlockedExchange(&s_fin_lock, 1)) Sleep(0);
}

static void fin_unlock(void)
{
    InterlockedExchange(&s_fin_lock, 0);
}

/* A free slot, after closing the events of the files already finished: -1 while the exit runs, or
   while REC_FINISHING files are still being written. It marks nothing: the slot is filled by the
   recording's end (rec_end). */
static int fin_take(void)
{
    int i, slot = -1;
    fin_lock();
    for (i = 0; i < REC_FINISHING && !s_fin_exiting; i++) {
        if (s_fin[i] && WaitForSingleObject(s_fin[i], 0) == WAIT_OBJECT_0) {
            CloseHandle(s_fin[i]);
            s_fin[i] = NULL;
        }
        if (!s_fin[i] && slot < 0) slot = i;
    }
    fin_unlock();
    return slot;
}

/* 1 when a recording could take a slot now (fin_take): what a fork's readiness asks */
static int fin_free(void)
{
    return fin_take() >= 0;
}

void tagpu_rec_exit(void)
{
    HANDLE h[REC_FINISHING];
    DWORD n = 0;
    int i;
    fin_lock();
    s_fin_exiting = 1;
    for (i = 0; i < REC_FINISHING; i++)
        if (s_fin[i] && WaitForSingleObject(s_fin[i], 0) != WAIT_OBJECT_0) h[n++] = s_fin[i];
    fin_unlock();
    if (!n) return;
    tagpu_logf("recorder: the exit waits for %lu recording file%s to be finished", n,
               n == 1 ? "" : "s");
    WaitForMultipleObjects(n, h, TRUE, INFINITE);
    tagpu_logf("recorder: the recording files are finished");
}

/* ---- the start and the end ------------------------------------------------------------------ */

static void rec_free(Rec* r)
{
    unsigned i;
    if (!r) return;
    for (i = 0; r->tab && i < r->ntab; i++) {
        if (r->tab[i].shadow) VirtualFree(r->tab[i].shadow, 0, MEM_RELEASE);
        if (r->tab[i].raw) VirtualFree(r->tab[i].raw, 0, MEM_RELEASE);
        if (r->tab[i].pages) HeapFree(GetProcessHeap(), 0, r->tab[i].pages);
    }
    if (r->tab) HeapFree(GetProcessHeap(), 0, r->tab);
    if (r->rowbuf) HeapFree(GetProcessHeap(), 0, r->rowbuf);
    if (r->ushadow) VirtualFree(r->ushadow, 0, MEM_RELEASE);
    if (r->uraw) VirtualFree(r->uraw, 0, MEM_RELEASE);
    if (r->ulive) HeapFree(GetProcessHeap(), 0, r->ulive);
    if (r->oscratch) VirtualFree(r->oscratch, 0, MEM_RELEASE);
    if (r->oheap) HeapDestroy(r->oheap);             /* every list's image with it */
    feat_free(r->feat);
    if (r->jraw) VirtualFree(r->jraw, 0, MEM_RELEASE);
    if (r->jshadow) VirtualFree(r->jshadow, 0, MEM_RELEASE);
    if (r->mshadow) VirtualFree(r->mshadow, 0, MEM_RELEASE);
    if (r->wake) CloseHandle(r->wake);
    if (r->done) CloseHandle(r->done);
    HeapFree(GetProcessHeap(), 0, r);
}

static void ring_free(RecRing* g)
{
    if (!g) return;
    if (g->fork.file) CloseHandle(g->fork.file);    /* THE FORK'S HANDLE: the ring owned it */
    if (g->buf) VirtualFree(g->buf, 0, MEM_RELEASE);
    if (g->grid) VirtualFree(g->grid, 0, MEM_RELEASE);
    if (g->wake) CloseHandle(g->wake);
    if (g->done) CloseHandle(g->done);
    HeapFree(GetProcessHeap(), 0, g);
}

/* the capture's own duplicate of one of the ring's events, or NULL */
static HANDLE dup_event(HANDLE e)
{
    HANDLE d = NULL;
    if (e && !DuplicateHandle(GetCurrentProcess(), e, GetCurrentProcess(), &d, 0, FALSE,
                              DUPLICATE_SAME_ACCESS))
        d = NULL;
    return d;
}

/* Everything a recording will need, taken now. After this the capture allocates only from its own
   heap, for the order lists' images, and for a heap table that grows (table_grow). */
static Rec* rec_start(const Level* L, int tick)
{
    HANDLE heap = GetProcessHeap();
    Rec* r = (Rec*)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof *r);
    RecRing* g = (RecRing*)HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof *g);
    unsigned i, n = tagpu_simreg_count(), widest = 4;
    const SimDesc* wide = table_too_wide();
    char map[64];
    SYSTEMTIME lt;
    FILETIME now;
    Tlv t;
    static unsigned char hb[HEADER_MAX];

    if (!r || !g) goto fail;
    if (s_fork.file) {                  /* THE FORK'S HANDLE moves to the ring */
        g->fork = s_fork;
        s_fork.file = NULL;
    }
    if (wide) {
        tagpu_logf("recorder: FAILED %s has rows of %u bytes, past the %u a record carries: not "
                   "recording", wide->name, wide->stride, SPAN_MAX);
        goto fail;
    }
    r->fin = fin_take();
    if (r->fin < 0) {
        tagpu_logf("recorder: not recording: the process is exiting, or %u earlier recordings' "
                   "files are still being written", REC_FINISHING);
        goto fail;
    }
    g->buf = (unsigned char*)VirtualAlloc(NULL, REC_RING_BYTES, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    g->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    g->done = CreateEventA(NULL, TRUE, FALSE, NULL);
    r->wake = dup_event(g->wake);
    r->done = dup_event(g->done);
    r->ring = g;
    r->begin = L->begin;
    r->nslots = L->nslots;
    r->tick = tick - 1;
    r->first = 1;
    r->L = *L;
    r->tid = GetCurrentThreadId();
    r->ushadow = (unsigned char*)VirtualAlloc(NULL, (size_t)L->nslots * REC_UNIT_IMAGE,
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->uraw = (unsigned char*)VirtualAlloc(NULL, (size_t)L->nslots * REC_UNIT_IMAGE,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->ulive = (unsigned char*)HeapAlloc(heap, HEAP_ZERO_MEMORY, L->nslots);
    r->tab = (Table*)HeapAlloc(heap, HEAP_ZERO_MEMORY, (n ? n : 1) * sizeof *r->tab);
    /* the lists' images live in a heap of their own, which only this thread uses and which the
       recording's end destroys whole */
    r->oheap = HeapCreate(HEAP_NO_SERIALIZE, 0, 0);
    r->olist = r->oheap ? (OList**)HeapAlloc(r->oheap, HEAP_ZERO_MEMORY,
                                             (size_t)L->nslots * 2 * sizeof *r->olist) : NULL;
    r->oscratch = (unsigned char*)VirtualAlloc(NULL, ORD_SCRATCH, MEM_COMMIT | MEM_RESERVE,
                                               PAGE_READWRITE);
    if (!g->buf || !g->wake || !r->wake || !g->done || !r->done || !r->ushadow || !r->uraw ||
        !r->ulive || !r->tab || !r->olist || !r->oscratch)
        goto fail;
    r->feat = feat_start(L->ta, g);
    if (!r->feat) goto fail;
    r->jpool = (const unsigned char*)L->proj;
    if (!r->jpool) {
        tagpu_logf("recorder: FAILED the level has no projectile pool: not recording");
        goto fail;
    }
    r->jraw = (unsigned char*)VirtualAlloc(NULL, (size_t)TAGPU_LIM_PROJ * REC_PROJ_BYTES,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->jshadow = (unsigned char*)VirtualAlloc(NULL, (size_t)TAGPU_LIM_PROJ * REC_PROJ_IMAGE,
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!r->jraw || !r->jshadow) goto fail;
    r->mmap = *(const unsigned char* const*)(L->ta + OFF_MAPPED);
    r->mbytes = mapped_bytes(L->ta);
    if (!r->mmap || !r->mbytes) {
        tagpu_logf("recorder: FAILED the level has no explored map: not recording");
        goto fail;
    }
    r->mshadow = (unsigned char*)VirtualAlloc(NULL, r->mbytes, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE);
    if (!r->mshadow) goto fail;
    for (i = 0; i < REC_PLAYERS; i++)
        r->ctl[i] = (unsigned char)L->ta[OFF_PLAYERS + i * PL_STRIDE + PL_CONTROLLER];
    r->view[0] = (unsigned char)L->ta[OFF_CONTROLLED];
    r->view[1] = (unsigned char)L->ta[OFF_VIEWED];
    for (i = 0; i < n; i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        Table* tb;
        unsigned bytes;
        if (!d || !table_recorded(d)) continue;
        if (r->ntab == REC_TABLE_MAX) {
            tagpu_logf("recorder: FAILED more than %u declared tables to record: not recording",
                       REC_TABLE_MAX);
            goto fail;
        }
        tb = &r->tab[r->ntab++];
        tb->d = d;
        tb->index = i;
        tb->zero = image_zero(d);
        tb->lhash = tagpu_simreg_layout_hash(d);
        table_rows(d, &bytes);
        /* the rows a declaration states, or for a heap table that counts its own, what it holds
           now: cap_tables grows the room when that table grows */
        if (!table_grow(tb, d->nrows ? d->stride * d->nrows : bytes)) goto fail;
        if (d->stride > widest) widest = d->stride;
    }
    r->rowbuf = (unsigned char*)HeapAlloc(heap, HEAP_ZERO_MEMORY, widest);
    if (!r->rowbuf) goto fail;

    map_stem(L->ta, map, sizeof map);
    GetLocalTime(&lt);
    GetSystemTimeAsFileTime(&now);
    if (g->fork.file)
        snprintf(g->path, sizeof g->path, "%s", g->fork.path);
    else
        snprintf(g->path, sizeof g->path, "replays\\%04u-%02u-%02u %02u%02u%02u %s.imprec",
                 lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, map);
    t.b = hb;
    t.n = 0;
    t.cap = sizeof hb;
    t.full = 0;
    header_build(L, tick, map, &now, g->fork.file ? &g->fork : NULL, &t);
    header_tables(r, &t);
    for (i = 0; i < REC_PLAYERS; i++)        /* the seats header_build names (RT_PLAYER) */
        if (L->ta[OFF_PLAYERS + i * PL_STRIDE + PL_CONTROLLER]) r->seated |= (unsigned short)(1u << i);
    if (t.full) {
        tagpu_logf("recorder: FAILED the header does not fit in %u bytes: not recording", t.cap);
        goto fail;
    }
    if (!ring_put(r, RK_HEADER, NULL, 0, hb, t.n)) goto fail;
    {
        unsigned n = minimap_build(L->ta, hb);
        if (n && !ring_put(r, RK_MINIMAP, NULL, 0, hb, n)) goto fail;
    }
    ring_publish(r, 1);
    if (!tagpu_recwriter_start(g)) {
        tagpu_logf("recorder: FAILED no writer thread (%lu): not recording", GetLastError());
        goto fail;
    }
    if (g->fork.id)
        tagpu_logf("recorder: recording timeline %u of %s from GameTime %d (parent %u): %u slots, "
                   "%u tables", g->fork.id, g->path, tick, g->fork.parent, L->nslots, r->ntab);
    else
        tagpu_logf("recorder: recording %s from GameTime %d: %u slots, %u tables", g->path, tick,
                   L->nslots, r->ntab);
    return r;

fail:
    ring_free(g);
    rec_free(r);
    return NULL;
}

/* The game's end (main+0x3923B, a u16 of flags: bit 4 won, bit 6 lost, bit 2 stop; the others
   are the frame's own) and each seat's RT_SEAT_END, by tagpu_recfmt.h's RecResult rule, into `b`.
   The flags still hold the result at the teardown's entry, where a level's recording ends (level
   init clears bits 2, 4, 5 and 6, 0x491A06..0x491A30); a recording that ends otherwise has none. */
#define OFF_GAME_END 0x3923B
#define GAME_END_BITS 0x74u
static void seats_end(const Rec* r, unsigned reason, unsigned char* b)
{
    const char* ta = r->L.ta;
    unsigned flags = reason == REC_END_TEARDOWN || reason == REC_END_RING
                         ? *(const unsigned short*)(ta + OFF_GAME_END) & GAME_END_BITS : 0, i;
    unsigned c = (unsigned char)ta[OFF_CONTROLLED];
    const char* crec = c < REC_PLAYERS ? ta + OFF_PLAYERS + (size_t)c * PL_STRIDE : NULL;
    memcpy(b, &flags, 4);
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned char* e = b + 4 + 12 * i;
        unsigned char res = REC_RESULT_UNKNOWN;
        /* a seat that was given units again (a scenario's load, a Deathmatch respawn) was not
           defeated: only a live count of 0 at the end is */
        unsigned defeat = *(const unsigned short*)(ta + OFF_PLAYERS + (size_t)i * PL_STRIDE + PL_LIVE)
                              ? 0 : r->defeat[i];
        if (!(r->seated >> i & 1)) res = REC_RESULT_NONE;
        else if (defeat) res = REC_RESULT_LOST;
        else if ((flags & 0x10) && crec && (i == c || crec[PL_ALLY + i])) res = REC_RESULT_WON;
        else if ((flags & 0x10) && crec) res = REC_RESULT_LOST;
        else if ((flags & 0x40) && i == c) res = REC_RESULT_LOST;
        e[0] = (unsigned char)i;
        e[1] = res;
        e[2] = e[3] = 0;
        memcpy(e + 4, &r->cmdr[i], 4);
        memcpy(e + 8, &defeat, 4);
    }
}

/* The capture's last act on a recording: RK_END, published; the ring is the writer's from here
   (the hand-over, tagpu_recorder.h), and the writer's `done` goes to the exit's list. What was
   never published is dropped: an unfinished iteration's records, and a step's events when the
   recording ends before the next capture's checks pass (a step's events are published by that
   capture, before its images, and by the teardown's end). The ring's reserve holds the end
   whatever the published part left. The event hooks are off before this runs (s_tid). */
static void rec_end(Rec* r, unsigned reason)
{
    unsigned char b[REC_END_BYTES], * at;
    unsigned short why = (unsigned short)reason, z = 0;
    unsigned pos, off = (unsigned)InterlockedCompareExchange(&s_offthread, 0, 0);
    unsigned unobserved = feat_unobserved(r);
    s_rec_fired = 0;
    r->w = (unsigned)InterlockedCompareExchange(&r->ring->wpub, 0, 0);
    memcpy(b, &why, 2);
    memcpy(b + 2, &z, 2);
    memcpy(b + 4, &s_bad, 4);
    memcpy(b + 8, &r->high, 4);
    memcpy(b + 12, &off, 4);
    memcpy(b + 16, &r->uncredited, 4);
    memcpy(b + 20, &unobserved, 4);
    seats_end(r, reason, b + 24);
    pos = r->w & (REC_RING_BYTES - 1);
    at = r->ring->buf + pos;
    if (pos + 4 + sizeof b > REC_RING_BYTES) {
        RecHdr h = { RK_PAD, 0 };
        memcpy(at, &h, 4);
        r->w += REC_RING_BYTES - pos;
        at = r->ring->buf;
    }
    memcpy(at + 4, b, sizeof b);
    ring_commit(r, at, RK_END, sizeof b);
    InterlockedExchange(&r->ring->wend, (LONG)r->w);
    InterlockedExchange(&r->ring->ended, 1);
    InterlockedExchange(&r->ring->wpub, (LONG)r->w);    /* the last access to the ring */
    r->ring = NULL;
    SetEvent(r->wake);
    fin_lock();
    if (!s_fin_exiting) {
        s_fin[r->fin] = r->done;
        r->done = NULL;
    }
    fin_unlock();
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        tagpu_logf("recorder: %u iterations captured: the first in %u us, the rest in %u us on "
                   "average and %u us at most (GameTime %d, %u bytes into the ring); after the "
                   "first, the simulation wrote %u write-watched pages, at most %u in one "
                   "iteration", r->iterations, (unsigned)(r->cost_first * 1000000 / f.QuadPart),
                   r->iterations > 1 ? (unsigned)(r->cost * 1000000 / f.QuadPart
                   / (r->iterations - 1)) : 0, (unsigned)(r->cost_max * 1000000 / f.QuadPart),
                   r->max_tick, r->max_bytes, r->pages, r->pages_max);
    }
    tagpu_logf("recorder: ended after GameTime %d: %s; %u id rows outside their tables, %u events "
               "refused off the game thread, %u nano sprays uncredited, %u order nodes whose owner "
               "or list flag disagreed with the list holding them", r->tick,
               reason == REC_END_TEARDOWN ? "the level ended" :
               reason == REC_END_RING     ? "INCOMPLETE, the ring was full" :
               reason == REC_END_WRITE    ? "INCOMPLETE, the file could not be written" :
               reason == REC_END_TABLE    ? "INCOMPLETE, no memory for a declared table's growth" :
               reason == REC_END_LEVEL    ? "INCOMPLETE, the level changed without a teardown" :
               reason == REC_END_ORDERS   ? "INCOMPLETE, an order list could not be recorded" :
               reason == REC_END_FEATURES ? "INCOMPLETE, the features are not the ones it began "
                                            "with, or their calls nested deeper than it tracks" :
               reason == REC_END_PROJ     ? "INCOMPLETE, the projectile pool is not the one it "
                                            "began with, or its count is past its records" :
               reason == REC_END_MAPPED   ? "INCOMPLETE, the explored map is not the one it began "
                                            "with" :
                                            "INCOMPLETE, the game stopped being a skirmish", s_bad,
               off, r->uncredited, r->oodd);
    feat_log(r);
    tagpu_logf("recorder: the projectiles: %u record images; the pool's written extent reached %u "
               "of its %u records", r->jimages, r->jhw, (unsigned)TAGPU_LIM_PROJ);
    tagpu_logf("recorder: the explored map: %u bytes; %u iterations changed it, %llu bytes in "
               "their runs", r->mbytes, r->mruns, r->mrun_bytes);
    rec_free(r);
}

/* ---- one iteration -------------------------------------------------------------------------- */

/* Every slot: a unit that died goes, with its order lists; a unit whose record's or mover's bytes
   moved is imaged, and its image's changed dwords go into the ring. A unit whose bytes did not move
   keeps the image it had: an image is a function of those bytes alone within a recording, since
   the tables its IDs index -- the unit array, the defs, the players, the weapons, the buckets -- do
   not move until the level ends, which ends the recording. */
static int cap_units(Rec* r, const Level* L)
{
    unsigned slot, img[REC_UNIT_DWORDS];
    for (slot = 1; slot < L->nslots; slot++) {
        unsigned* sh = (unsigned*)(r->ushadow + (size_t)slot * REC_UNIT_IMAGE);
        const unsigned char* rec = (const unsigned char*)L->begin + (size_t)slot * REC_UNIT_BYTES;
        const unsigned char* mv;
        unsigned char* raw = r->uraw + (size_t)slot * REC_UNIT_IMAGE;
        if (!unit_live(L, slot)) {
            if (r->ulive[slot]) {
                unsigned short pre[2] = { (unsigned short)slot, 0 };
                if (!ring_put(r, RK_UNIT_GONE, pre, 4, NULL, 0)) return 0;
                r->ulive[slot] = 0;
                olist_drop(r, slot);
            }
            continue;
        }
        mv = *(const unsigned char* const*)(rec + U_MOVER);
        if (r->ulive[slot] && diff_from(rec, raw, 0, REC_UNIT_BYTES) == REC_UNIT_BYTES &&
            (!mv || diff_from(mv, raw + REC_MOVER_AT, 0, REC_MOVER_BYTES) == REC_MOVER_BYTES))
            continue;
        unit_raw(L, slot, raw);
        unit_image_raw(L, raw, img);
        /* a new unit, or a slot whose type changed, goes whole: nothing of its past applies */
        if (!r->ulive[slot] ||
            memcmp((unsigned char*)img + U_TYPE, (unsigned char*)sh + U_TYPE, 4)) {
            unsigned short pre[2] = { (unsigned short)slot, 0 };
            if (!ring_put(r, RK_UNIT, pre, 4, img, REC_UNIT_IMAGE)) return 0;
        } else {
            unsigned char bits[REC_UNIT_DELTA_HEAD - 2] = { 0 }, * at;
            unsigned short s = (unsigned short)slot;
            unsigned i, k = 0, n = 0;
            for (i = 0; i < REC_UNIT_DWORDS; i++)
                if (img[i] != sh[i]) { bits[i >> 3] |= (unsigned char)(1u << (i & 7)); n++; }
            if (!n) continue;
            at = ring_take(r, REC_UNIT_DELTA_HEAD + 4 * n);
            if (!at) return 0;
            memcpy(at + 4, &s, 2);
            memcpy(at + 6, bits, sizeof bits);
            for (i = 0; i < REC_UNIT_DWORDS; i++)
                if (img[i] != sh[i]) memcpy(at + 4 + REC_UNIT_DELTA_HEAD + 4 * k++, &img[i], 4);
            ring_commit(r, at, RK_UNIT_DELTA, REC_UNIT_DELTA_HEAD + 4 * n);
        }
        memcpy(sh, img, REC_UNIT_IMAGE);
        r->ulive[slot] = 1;
    }
    return 1;
}

/* The ten players, and then their controllers and the two players the local game commands and
   views, which the images leave out, when they changed. */
static int cap_players(Rec* r, const Level* L)
{
    unsigned i, k, img[REC_PLAYER_DWORDS];
    unsigned char v[4];
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned* sh = r->pshadow[i];
        player_image(L, i, img);
        if (r->first) {
            unsigned short pre[2] = { (unsigned short)i, 0 };
            if (!ring_put(r, RK_PLAYER, pre, 4, img, sizeof img)) return 0;
        } else {
            unsigned char bits[REC_PLAYER_DELTA_HEAD - 2] = { 0 }, * at;
            unsigned short s = (unsigned short)i;
            unsigned j = 0, n = 0;
            for (k = 0; k < REC_PLAYER_DWORDS; k++)
                if (img[k] != sh[k]) { bits[k >> 3] |= (unsigned char)(1u << (k & 7)); n++; }
            if (!n) continue;
            at = ring_take(r, REC_PLAYER_DELTA_HEAD + 4 * n);
            if (!at) return 0;
            memcpy(at + 4, &s, 2);
            memcpy(at + 6, bits, sizeof bits);
            for (k = 0; k < REC_PLAYER_DWORDS; k++)
                if (img[k] != sh[k]) memcpy(at + 4 + REC_PLAYER_DELTA_HEAD + 4 * j++, &img[k], 4);
            ring_commit(r, at, RK_PLAYER_DELTA, REC_PLAYER_DELTA_HEAD + 4 * n);
        }
        memcpy(sh, img, sizeof img);
    }
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned char c = (unsigned char)L->ta[OFF_PLAYERS + i * PL_STRIDE + PL_CONTROLLER];
        if (c == r->ctl[i]) continue;
        v[0] = (unsigned char)i;
        v[1] = c;
        v[2] = v[3] = 0;
        if (!ring_put(r, RK_CONTROLLER, v, 4, NULL, 0)) return 0;
        r->ctl[i] = c;
    }
    v[0] = (unsigned char)L->ta[OFF_CONTROLLED];
    v[1] = (unsigned char)L->ta[OFF_VIEWED];
    v[2] = v[3] = 0;
    if (v[0] != r->view[0] || v[1] != r->view[1]) {
        if (!ring_put(r, RK_VIEWER, v, 4, NULL, 0)) return 0;
        r->view[0] = v[0];
        r->view[1] = v[1];
    }
    return 1;
}

/* ---- the order lists ------------------------------------------------------------------------ */

enum { OL_OK, OL_TOO_LONG, OL_NOT_A_NODE };

/* One of a live unit's lists (0 the main list, 1 the sub list), its nodes' images into `out`, and
   how many. The head of this file says why each read is sound; a node that fails the vtable test,
   or a list past REC_ORDERS_MAX, is refused rather than read on. A node whose owner or list flag
   disagrees with the list holding it is counted in `*odd`, when given: the engine's linkers keep
   both, so one is a fact about the engine to look into, not a reason to stop. */
static int olist_read(const Level* L, unsigned slot, unsigned list, unsigned char* out,
                      unsigned* count, unsigned* odd)
{
    const char* unit = L->begin + (size_t)slot * REC_UNIT_BYTES;
    const char* node = *(const char* const*)(unit + (list ? U_SUBORDERS : U_ORDERS));
    unsigned n = 0;
    unsigned char raw[REC_ORDER_IMAGE];
    for (; node; node = *(const char* const*)(node + ORD_NEXT)) {
        unsigned flags;
        if (n == REC_ORDERS_MAX) return OL_TOO_LONG;
        if (*(const unsigned*)node != ORD_VTABLE) return OL_NOT_A_NODE;
        memcpy(&flags, node + ORD_FLAGS, 4);
        if (odd && (*(const char* const*)(node + ORD_UNIT) != unit ||
                    !(flags & ORD_SUBLIST) != !list))
            (*odd)++;
        memcpy(raw, node, REC_ORDER_BYTES);
        memset(raw + REC_ORDER_BYTES, 0, REC_ORDER_IMAGE - REC_ORDER_BYTES);
        image(L, raw, REC_ORDER_IMAGE / 4, s_omask, IMG_ORDER, (unsigned*)(out + (size_t)n * REC_ORDER_IMAGE));
        n++;
    }
    *count = n;
    return OL_OK;
}

/* a list's records: several when it is longer than one carries, one when it is empty */
static int olist_put(Rec* r, unsigned kind, unsigned slot, unsigned list, const unsigned char* img,
                     unsigned n)
{
    unsigned first = 0;
    do {
        unsigned char pre[8];
        unsigned short s = (unsigned short)slot, len = (unsigned short)n, f = (unsigned short)first;
        unsigned k = n - first < REC_ORDERS_CHUNK ? n - first : REC_ORDERS_CHUNK;
        memcpy(pre, &s, 2);
        pre[2] = (unsigned char)list;
        pre[3] = 0;
        memcpy(pre + 4, &len, 2);
        memcpy(pre + 6, &f, 2);
        if (!ring_put(r, kind, pre, 8, img + (size_t)first * REC_ORDER_IMAGE, k * REC_ORDER_IMAGE))
            return 0;
        first += k;
    } while (first < n);
    return 1;
}

static void olist_drop(Rec* r, unsigned slot)
{
    unsigned k;
    for (k = 0; k < 2; k++) {
        if (r->olist[slot * 2 + k]) HeapFree(r->oheap, 0, r->olist[slot * 2 + k]);
        r->olist[slot * 2 + k] = NULL;
    }
}

static void olist_fail(unsigned slot, unsigned list, int why)
{
    tagpu_logf("recorder: slot %u's %s list %s (%u nodes at most): the recording ends here", slot,
               list ? "sub" : "main",
               why == OL_TOO_LONG   ? "is too long to record" :
               why == OL_NOT_A_NODE ? "reaches a node whose vtable is not an order's" :
                                      "has no memory for its image", REC_ORDERS_MAX);
}

/* Every live unit's two lists: the whole list, when its image changed. 0 when the ring is full,
   -1 when a list cannot be recorded (REC_END_ORDERS). */
static int cap_orders(Rec* r, const Level* L)
{
    unsigned slot, list, n;
    for (slot = 1; slot < L->nslots; slot++) {
        if (!r->ulive[slot]) continue;
        for (list = 0; list < 2; list++) {
            OList** at = &r->olist[slot * 2 + list];
            OList* ol = *at;
            int why = olist_read(L, slot, list, r->oscratch, &n, &r->oodd);
            if (why != OL_OK) { olist_fail(slot, list, why); return -1; }
            if (ol ? ol->n == n && !memcmp(ol->img, r->oscratch, (size_t)n * REC_ORDER_IMAGE) : !n)
                continue;
            if (!olist_put(r, RK_ORDERS, slot, list, r->oscratch, n)) return 0;
            if (!n) {
                HeapFree(r->oheap, 0, ol);
                *at = NULL;
                continue;
            }
            if (!ol || ol->cap < n) {
                unsigned cap = n < 4 ? 4 : n;
                size_t bytes = sizeof *ol + (size_t)cap * REC_ORDER_IMAGE;
                OList* g = ol ? (OList*)HeapReAlloc(r->oheap, 0, ol, bytes)
                              : (OList*)HeapAlloc(r->oheap, 0, bytes);
                if (!g) { olist_fail(slot, list, -1); return -1; }
                g->cap = cap;
                *at = ol = g;
            }
            ol->n = n;
            memcpy(ol->img, r->oscratch, (size_t)n * REC_ORDER_IMAGE);
        }
    }
    return 1;
}

static int put_span(Rec* r, unsigned table, unsigned off, const unsigned char* p, unsigned n)
{
    unsigned char pre[8];
    unsigned short ix = (unsigned short)table, z = 0;
    memcpy(pre, &ix, 2);
    memcpy(pre + 2, &z, 2);
    memcpy(pre + 4, &off, 4);
    return ring_put(r, RK_TABLE_SPAN, pre, 8, p, n);
}

/* A table's room: its shadow, its raw copy when it has one and a heap table's page list, grown to
   hold `bytes`. Taken at the start; after it, only a heap table that counts its own rows grows,
   at most once a level: the extra weapons' side table is allocated for the level's unit array at
   the first unit with more than three weapons (tagpu_weapons.c, side_row). */
static int table_grow(Table* tb, unsigned bytes)
{
    unsigned cap = (bytes + 0xFFFFu) & ~0xFFFFu;
    unsigned char *n, *w = NULL;
    PVOID* pg = NULL;
    if (!bytes || cap <= tb->cap) return 1;
    n = (unsigned char*)VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (n && tb->d->layout && !(tb->d->flags & SIMD_HEAP) && !tb->zero)
        w = (unsigned char*)VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (n && (tb->d->flags & SIMD_HEAP))
        pg = (PVOID*)HeapAlloc(GetProcessHeap(), 0, (cap / 4096 + 2) * sizeof *pg);
    if (!n || (tb->d->layout && !(tb->d->flags & SIMD_HEAP) && !tb->zero && !w) ||
        ((tb->d->flags & SIMD_HEAP) && !pg)) {
        tagpu_logf("recorder: no memory for %s's %u bytes", tb->d->name, bytes);
        if (n) VirtualFree(n, 0, MEM_RELEASE);
        if (w) VirtualFree(w, 0, MEM_RELEASE);
        if (pg) HeapFree(GetProcessHeap(), 0, pg);
        return 0;
    }
    if (tb->size) memcpy(n, tb->shadow, tb->size);
    if (tb->size && w) memcpy(w, tb->raw, tb->size);
    if (tb->shadow) VirtualFree(tb->shadow, 0, MEM_RELEASE);
    if (tb->raw) VirtualFree(tb->raw, 0, MEM_RELEASE);
    if (tb->pages) HeapFree(GetProcessHeap(), 0, tb->pages);
    tb->shadow = n;
    tb->raw = w;
    tb->pages = pg;
    tb->npages = pg ? cap / 4096 + 2 : 0;
    tb->cap = cap;
    return 1;
}

/* The bytes a heap table's allocation holds from `base`, by the OS's own account of the region:
   a bound on its rows besides its count, so a count that says more cannot take a read past it. */
static unsigned heap_limit(const unsigned char* base)
{
    MEMORY_BASIC_INFORMATION m;
    if (!base || !VirtualQuery(base, &m, sizeof m) || m.State != MEM_COMMIT) return 0;
    return (unsigned)((const unsigned char*)m.BaseAddress + m.RegionSize - base);
}

/* One row: its image against the shadow, and the image's changed runs into the ring, bridging
   unchanged gaps of up to SPAN_GAP bytes. With a raw copy, the image is taken from the copy, so
   it is the image of exactly the bytes the next capture compares with. */
static int row_emit(Rec* r, const Level* L, Table* tb, unsigned row)
{
    const SimDesc* d = tb->d;
    const unsigned char* src = tb->base + (size_t)row * d->stride;
    const unsigned char* img = s_zero;
    unsigned char* sh = tb->shadow + (size_t)row * d->stride;
    unsigned i = 0, off0 = row * d->stride;
    if (tb->raw) {
        memcpy(tb->raw + off0, src, d->stride);
        src = tb->raw + off0;
    }
    if (diff_from(src, s_zero, 0, d->stride) < d->stride) {
        row_image(L, d, src, r->rowbuf);
        img = r->rowbuf;
    }
    if (!memcmp(img, sh, d->stride)) return 1;
    while (i < d->stride) {
        unsigned a, b, gap;
        if (img[i] == sh[i]) { i++; continue; }
        a = i;
        b = i + 1;
        for (gap = 0, i++; i < d->stride && gap <= SPAN_GAP; i++) {
            if (img[i] != sh[i]) { b = i + 1; gap = 0; } else gap++;
        }
        if (!put_span(r, tb->index, off0 + a, img + a, b - a)) return 0;
        i = b;
    }
    memcpy(sh, img, d->stride);
    return 1;
}

static int rows_emit(Rec* r, const Level* L, Table* tb, unsigned from, unsigned to)
{
    for (; from < to; from++)
        if (!row_emit(r, L, tb, from)) return 0;
    return 1;
}

/* The rows [0, rows) whose bytes differ from `was`, the copy the last capture read. */
static int rows_moved(Rec* r, const Level* L, Table* tb, const unsigned char* was, unsigned rows)
{
    unsigned at = 0, lim = rows * tb->d->stride;
    while ((at = diff_from(tb->base, was, at, lim)) < lim) {
        unsigned row = at / tb->d->stride;
        if (!row_emit(r, L, tb, row)) return 0;
        at = (row + 1) * tb->d->stride;
    }
    return 1;
}

/* Every row, when a table's copies say nothing about it: the first capture, a heap table that
   moved or that GetWriteWatch refuses. A table without a layout is its own image, so its shadow
   is the copy to compare with. A row of zeros images to zeros without imaging, so against a
   shadow still zero nothing of it goes into the ring. */
static int table_whole(Rec* r, const Level* L, Table* tb, unsigned rows)
{
    return tb->d->layout ? rows_emit(r, L, tb, 0, rows) : rows_moved(r, L, tb, tb->shadow, rows);
}

/* The rows whose bytes may have moved since the last capture, and only those, are imaged:
   - a static table's, found by comparing its bytes with the copy the last capture read;
   - a heap table's, on the pages the OS says were written since the last capture: SIM_ALLOC
     gives every heap table MEM_WRITE_WATCH, and GetWriteWatch answers and resets in one call.
     Only the game thread writes these tables during play, the thread this runs on, so no write
     falls between the answer and the reset;
   - every row, at the first capture, when a heap table has moved and for rows a table's count
     has just taken in: their copies say nothing about them.
   An image is a function of its row's bytes alone (the tables IDs index do not move within a
   recording), so a row whose bytes did not move has the image it had. The rotating check
   compares fresh images against the writer's copy regardless, and names any row this missed. */
static int cap_table(Rec* r, const Level* L, Table* tb)
{
    const SimDesc* d = tb->d;
    unsigned bytes, rows, old, whole = r->first;
    const unsigned char* base = table_rows(d, &bytes);
    if (d->flags & SIMD_HEAP) {
        if (base != tb->base) {
            tb->limit = heap_limit(base);
            tb->unwatched = 0;
            whole = 1;
        }
        if (bytes > tb->limit) {
            if (!tb->overcount)
                tagpu_logf("recorder: %s counts %u bytes, its allocation holds %u: the rows past "
                           "it are not recorded", d->name, bytes, tb->limit);
            tb->overcount = 1;
            bytes = tb->limit - tb->limit % d->stride;
        }
    }
    tb->base = base;
    if (bytes > tb->cap && !table_grow(tb, bytes)) return -1;
    old = tb->size / d->stride;
    if (bytes != tb->size || r->first) {
        unsigned char pre[8];
        unsigned short ix = (unsigned short)tb->index, z = 0;
        memcpy(pre, &ix, 2);
        memcpy(pre + 2, &z, 2);
        memcpy(pre + 4, &bytes, 4);
        if (!ring_put(r, RK_TABLE_SIZE, pre, 8, NULL, 0)) return 0;
        if (bytes > tb->size) {
            memset(tb->shadow + tb->size, 0, bytes - tb->size);
            if (tb->raw) memset(tb->raw + tb->size, 0, bytes - tb->size);
        }
        tb->size = bytes;
    }
    rows = bytes / d->stride;
    if (old > rows) old = rows;
    if (tb->zero) return 1;              /* the shadow's zeros are its image */
    if (d->flags & SIMD_HEAP) {
        ULONG_PTR n = tb->npages, k;
        DWORD gran;
        unsigned next = 0;
        if (!bytes) return 1;
        /* the watch reports whole pages, and a reset clears a page for every table on it, so only
           rows that start on a page are watched: any other base is diffed whole */
        if (tb->unwatched || ((size_t)base & 0xFFFu) ||
            GetWriteWatch(WRITE_WATCH_FLAG_RESET, (PVOID)base, bytes, tb->pages, &n, &gran)) {
            if (!tb->unwatched)
                tagpu_logf("recorder: %s is not write-watched: it is diffed whole each time",
                           d->name);
            tb->unwatched = 1;
            return table_whole(r, L, tb, rows) ? 1 : 0;
        }
        r->pages_now += (unsigned)n;
        if (whole) return table_whole(r, L, tb, rows) ? 1 : 0;
        for (k = 0; k < n; k++) {
            unsigned lo = (unsigned)((const unsigned char*)tb->pages[k] - base);
            unsigned a = lo / d->stride, b = (lo + gran + d->stride - 1) / d->stride;
            if (a < next) a = next;
            if (b > old) b = old;
            if (a < b && !rows_emit(r, L, tb, a, b)) return 0;
            if (b > next) next = b;
        }
        return rows_emit(r, L, tb, old, rows) ? 1 : 0;
    }
    if (whole) return table_whole(r, L, tb, rows) ? 1 : 0;
    if (!rows_moved(r, L, tb, tb->raw ? tb->raw : tb->shadow, old)) return 0;
    return rows_emit(r, L, tb, old, rows) ? 1 : 0;
}

/* 1 captured, 0 the ring is full, -1 a table's shadow could not grow */
static int cap_tables(Rec* r, const Level* L)
{
    unsigned t;
    r->pages_now = 0;
    for (t = 0; t < r->ntab; t++) {
        int k = cap_table(r, L, &r->tab[t]);
        if (k <= 0) return k;
    }
    return 1;
}

/* The rotating check: fresh images, straight from engine memory, of the next thirtieth of the unit
   slots with a live unit's two order lists, one player and the next VERIFY_TABLE bytes of the
   declared tables' rows, so that every unit slot is checked once every VERIFY_ROUND iterations and
   every row in turn. The writer compares them with its own copy, which only the ring has built.
   0 when the ring is full, -1 when a list cannot be read (as cap_orders). */
static int cap_verify(Rec* r, const Level* L)
{
    unsigned k, per = (L->nslots - 1 + VERIFY_ROUND - 1) / VERIFY_ROUND, total = 0, budget, skips;
    unsigned img[IMG_DWORDS];
    for (k = 0; k < per; k++) {
        unsigned short pre[2];
        unsigned list, n;
        int live;
        if (++r->vslot >= L->nslots) r->vslot = 1;
        live = unit_live(L, r->vslot);
        pre[0] = (unsigned short)r->vslot;
        pre[1] = (unsigned short)live;
        if (live) unit_image(L, r->vslot, img);
        if (!ring_put(r, RK_VERIFY_UNIT, pre, 4, img, live ? REC_UNIT_IMAGE : 0)) return 0;
        for (list = 0; live && list < 2; list++) {
            int why = olist_read(L, r->vslot, list, r->oscratch, &n, NULL);
            if (why != OL_OK) { olist_fail(r->vslot, list, why); return -1; }
            if (!olist_put(r, RK_VERIFY_ORDERS, r->vslot, list, r->oscratch, n)) return 0;
        }
    }
    {
        unsigned short pre[2] = { (unsigned short)r->vplayer, 0 };
        player_image(L, r->vplayer, img);
        if (!ring_put(r, RK_VERIFY_PLAYER, pre, 4, img, REC_PLAYER_DWORDS * 4)) return 0;
        r->vplayer = (r->vplayer + 1) % REC_PLAYERS;
    }
    for (k = 0; k < r->ntab; k++) total += r->tab[k].size;
    budget = total < VERIFY_TABLE ? total : VERIFY_TABLE;
    /* whole rows, at most VERIFY_CHUNK bytes a record; each record takes at least one row off
       the budget, and an empty table is passed over, at most once each round */
    for (skips = 0; budget && skips <= r->ntab; ) {
        Table* tb = &r->tab[r->vtab];
        const SimDesc* d = tb->d;
        unsigned bytes = tb->size, n, row;
        const unsigned char* base = tb->base;
        unsigned char* at;
        if (r->voff >= bytes) {
            r->vtab = (r->vtab + 1) % r->ntab;
            r->voff = 0;
            skips++;
            continue;
        }
        n = (budget < VERIFY_CHUNK ? budget : VERIFY_CHUNK) / d->stride;
        if (!n) n = 1;
        if (r->voff + n * d->stride > bytes) n = (bytes - r->voff) / d->stride;
        at = ring_take(r, 8 + n * d->stride);
        if (!at) return 0;
        {
            unsigned short ix = (unsigned short)tb->index, z = 0;
            memcpy(at + 4, &ix, 2);
            memcpy(at + 6, &z, 2);
            memcpy(at + 8, &r->voff, 4);
        }
        for (row = 0; row < n; row++)
            row_image(L, d, base + r->voff + (size_t)row * d->stride,
                      at + 12 + (size_t)row * d->stride);
        ring_commit(r, at, RK_VERIFY_TABLE, 8 + n * d->stride);
        r->voff += n * d->stride;
        budget -= n * d->stride < budget ? n * d->stride : budget;
    }
    return 1;
}

/* ---- the globals and the projectiles -------------------------------------------------------- */

/* THE GLOBALS AND THE PROJECTILES (the slice note's S3b).

   THE GLOBALS are the fields of the main block and .data that replay-globals.tsv lists, read into
   one image (tagpu_replay_globals.h): each slot at its dword, its bits outside its rows' masks
   zero. The generator checks every main slot inside the main block's REC_MAIN_BYTES and every data
   slot inside .data, so no read leaves either: the main block lives for the process (0x41D920
   allocates it, once, and nothing frees it) and .data is the image's own section. The image goes
   whole at the first iteration and as a delta of the dwords that changed after it.

   THE PROJECTILES are the pool *(main+0x141F7), TAGPU_LIM_PROJ records of 0x6B bytes (the
   allocation 0x499A30, which the limits table sizes whole or the process ends: tagpu_limits.h),
   and its count main+0x141F3. The count is DATA: one past the records ends the recording
   (REC_END_PROJ), as does a pool that is not the one the recording began with, whose records
   the shadows and the quarry IDs describe. A record is imaged through the layout's proj rows:
   every byte a value but the four pointers, each its table's ID by arithmetic, never a
   dereference.
   WHICH RECORDS: the written extent [0, hw). The allocation zeroes the pool (0x499A55), a record
   is written only at the count -- an append, then count++ -- or by the compaction 0x49AE20 below
   the old count, and every record written holds a non-NULL weapon at +0x00 (the init 0x49C740
   stores it at 0x49C752, and each append's only success path ends in the init: 0x49C9C0's at
   0x49CA32, the six other live builders' likewise; a burst copy copies a whole template, the
   compaction whole records). So the records ever written are [0, peak), peak the highest the count
   has been this level, and every record past them is zero. The count can peak inside a step and
   fall back before the next capture (the appends, then the compaction at the tick's end), so the
   extent is found by scanning on from the last hw while +0x00 is non-NULL, bounded by the pool:
   hw = max(count, that slot) -- exact, and usually one read. A record below hw goes out when its
   bytes moved since it was last imaged: whole the first time it is in the extent, else as a delta
   of its image unless that is no smaller; the count and hw (RK_PROJ_HEAD) when either changed. */
static const unsigned char* global_at(const char* ta, const ReplayGlobalSlot* g)
{
    return g->where == RG_MAIN ? (const unsigned char*)ta + g->offset
                               : (const unsigned char*)(size_t)DATA_BEGIN + g->offset;
}

static void globals_image(const char* ta, unsigned* img)
{
    unsigned i;
    memset(img, 0, REC_GLOBALS_DWORDS * 4);
    for (i = 0; i < REC_GLOBALS_SLOTS; i++) {
        const ReplayGlobalSlot* g = &tagpu_replay_globals[i];
        if (g->size <= 4) {
            unsigned v = 0;
            memcpy(&v, global_at(ta, g), g->size);
            img[g->dword] = v & (g->write | g->hold | g->react);
        } else {
            memcpy(img + g->dword, global_at(ta, g), g->size);   /* one row: every byte */
        }
    }
}

/* 1 captured, 0 the ring is full */
static int cap_globals(Rec* r, const char* ta)
{
    unsigned img[REC_GLOBALS_DWORDS], i;
    globals_image(ta, img);
    if (r->first) {
        if (!ring_put(r, RK_GLOBALS, NULL, 0, img, sizeof img)) return 0;
    } else {
        unsigned char bits[REC_GLOBALS_DELTA_HEAD] = { 0 }, * at;
        unsigned k = 0, n = 0;
        for (i = 0; i < REC_GLOBALS_DWORDS; i++)
            if (img[i] != r->gshadow[i]) { bits[i >> 3] |= (unsigned char)(1u << (i & 7)); n++; }
        if (n) {
            at = ring_take(r, REC_GLOBALS_DELTA_HEAD + 4 * n);
            if (!at) return 0;
            memcpy(at + 4, bits, sizeof bits);
            for (i = 0; i < REC_GLOBALS_DWORDS; i++)
                if (img[i] != r->gshadow[i])
                    memcpy(at + 4 + REC_GLOBALS_DELTA_HEAD + 4 * k++, &img[i], 4);
            ring_commit(r, at, RK_GLOBALS_DELTA, REC_GLOBALS_DELTA_HEAD + 4 * n);
        }
    }
    memcpy(r->gshadow, img, sizeof img);
    /* the rotating check's: once each VERIFY_ROUND iterations, the image read afresh */
    if (r->iterations % VERIFY_ROUND == 0) {
        globals_image(ta, img);
        if (!ring_put(r, RK_VERIFY_GLOBALS, NULL, 0, img, sizeof img)) return 0;
    }
    return 1;
}

/* a record's image: its 0x6B bytes and a zero pad, the pointers as IDs */
static void proj_image(const Level* L, const unsigned char* rec, unsigned* img)
{
    unsigned char raw[REC_PROJ_IMAGE];
    memcpy(raw, rec, REC_PROJ_BYTES);
    raw[REC_PROJ_BYTES] = 0;
    image(L, raw, REC_PROJ_DWORDS, s_jmask, IMG_PROJ, img);
}

/* The written extent from `from`: the first record at or after it whose weapon is NULL, or the
   pool's end, and never below the count (*The globals and the projectiles*). */
static unsigned proj_extent(const unsigned char* pool, unsigned from, unsigned count)
{
    unsigned hw = from;
    for (; hw < TAGPU_LIM_PROJ; hw++) {
        unsigned weapon;
        memcpy(&weapon, pool + (size_t)hw * REC_PROJ_BYTES, 4);
        if (!weapon) break;
    }
    return hw < count ? count : hw;
}

/* 1 captured, 0 the ring is full, -1 the pool is not the recording's or its count is past it */
static int cap_proj(Rec* r, const Level* L)
{
    const unsigned char* pool = (const unsigned char*)L->proj;
    unsigned count, hw, i, img[REC_PROJ_DWORDS];
    if (pool != r->jpool) return -1;
    memcpy(&count, L->ta + OFF_NPROJ, 4);
    if (count > TAGPU_LIM_PROJ) return -1;
    hw = proj_extent(pool, r->jhw, count);
    if (r->first || count != r->jcount || hw != r->jhw) {
        unsigned head[2] = { count, hw };
        if (!ring_put(r, RK_PROJ_HEAD, head, sizeof head, NULL, 0)) return 0;
    }
    for (i = 0; i < hw; i++) {
        const unsigned char* rec = pool + (size_t)i * REC_PROJ_BYTES;
        unsigned char* raw = r->jraw + (size_t)i * REC_PROJ_BYTES;
        unsigned* sh = (unsigned*)(r->jshadow + (size_t)i * REC_PROJ_IMAGE);
        int fresh = i >= r->jhw;
        unsigned k, n = 0;
        if (!fresh && !memcmp(rec, raw, REC_PROJ_BYTES)) continue;
        memcpy(raw, rec, REC_PROJ_BYTES);
        proj_image(L, raw, img);
        for (k = 0; k < REC_PROJ_DWORDS; k++) n += img[k] != sh[k];
        if (!fresh && !n) continue;
        if (fresh || REC_PROJ_DELTA_HEAD + 4 * n >= 4 + REC_PROJ_IMAGE) {
            unsigned short pre[2] = { (unsigned short)i, 0 };
            if (!ring_put(r, RK_PROJ, pre, 4, img, REC_PROJ_IMAGE)) return 0;
        } else {
            unsigned char bits[REC_PROJ_DELTA_HEAD - 2] = { 0 }, * at;
            unsigned short ix = (unsigned short)i;
            unsigned j = 0;
            for (k = 0; k < REC_PROJ_DWORDS; k++)
                if (img[k] != sh[k]) bits[k >> 3] |= (unsigned char)(1u << (k & 7));
            at = ring_take(r, REC_PROJ_DELTA_HEAD + 4 * n);
            if (!at) return 0;
            memcpy(at + 4, &ix, 2);
            memcpy(at + 6, bits, sizeof bits);
            for (k = 0; k < REC_PROJ_DWORDS; k++)
                if (img[k] != sh[k]) memcpy(at + 4 + REC_PROJ_DELTA_HEAD + 4 * j++, &img[k], 4);
            ring_commit(r, at, RK_PROJ_DELTA, REC_PROJ_DELTA_HEAD + 4 * n);
        }
        memcpy(sh, img, REC_PROJ_IMAGE);
        r->jimages++;
    }
    r->jcount = count;
    r->jhw = hw;
    /* the rotating check's: the count, hw and the next records, read afresh, so that every record
       of the extent is checked once each VERIFY_ROUND iterations */
    {
        unsigned per = (hw + VERIFY_ROUND - 1) / VERIFY_ROUND, k, pre[3];
        unsigned char* at;
        if (r->vproj >= hw) r->vproj = 0;
        k = hw - r->vproj < per ? hw - r->vproj : per;
        if (k > VERIFY_CHUNK / REC_PROJ_IMAGE) k = VERIFY_CHUNK / REC_PROJ_IMAGE;
        at = ring_take(r, 12 + k * REC_PROJ_IMAGE);
        if (!at) return 0;
        pre[0] = count;
        pre[1] = hw;
        pre[2] = r->vproj;
        memcpy(at + 4, pre, 12);
        for (i = 0; i < k; i++)
            proj_image(L, pool + (size_t)(r->vproj + i) * REC_PROJ_BYTES,
                       (unsigned*)(at + 16 + (size_t)i * REC_PROJ_IMAGE));
        ring_commit(r, at, RK_VERIFY_PROJ, 12 + k * REC_PROJ_IMAGE);
        r->vproj += k;
    }
    return 1;
}

/* THE EXPLORED MAP *(main+0x14273): a u16 for each 2 x 2 cells of the feature grid, a bit a
   player (the setter 0x481930 sets 1 << [player+0x146]), REC_MAPPED_BYTES of the grid's columns
   and rows, allocated by LoadMap (0x483CF6..0x483D25) and freed only with the level. It only grows
   in play -- the setter stores 0 -> 1 after testing the bit (0x481A56, 0x481B64, 0x481CD1), and
   only the sight rebuild 0x4816A0 with Mapping on clears one -- so no iteration's state derives
   it, and the simulation reads it (the transport unload 0x47E37B, the AI's map, unit visibility
   0x465AC0 while LosType bit 1 is clear). So the recording carries it every iteration: whole at
   the first (RK_MAPPED), then the runs of bytes that changed (RK_MAPPED_RUNS), found by comparing
   the whole map with the copy the ring last carried -- one memcmp an iteration, and runs only when
   a bit was set. With Mapping off it is 0xFF everywhere from the load and never written. The map
   is the one the recording began with, at the size it began with, or the recording ends
   (REC_END_MAPPED). */
static unsigned mapped_bytes(const char* ta)
{
    unsigned w = *(const unsigned*)(ta + OFF_GRID_W), h = *(const unsigned*)(ta + OFF_GRID_H);
    return w && h && w <= 0xFFFF && h <= 0xFFFF ? REC_MAPPED_BYTES(w, h) : 0;
}

/* the map whole, as RK_MAPPED records from offset 0 (ring, or dump when `r` is NULL) */
static int mapped_whole(Rec* r, const unsigned char* map, unsigned bytes)
{
    unsigned off;
    for (off = 0; off < bytes; off += REC_MAPPED_CHUNK) {
        unsigned n = bytes - off < REC_MAPPED_CHUNK ? bytes - off : REC_MAPPED_CHUNK;
        if (r) {
            if (!ring_put(r, RK_MAPPED, &off, 4, map + off, n)) return 0;
        } else {
            dump_rec(RK_MAPPED, &off, 4, map + off, n);
        }
    }
    return 1;
}

#define MAPPED_GAP 8                /* unchanged bytes a run bridges rather than splitting       */

/* 1 captured, 0 the ring is full, -1 the map is not the recording's */
static int cap_mapped(Rec* r, const Level* L)
{
    unsigned char* runs = r->mrunbuf;
    const unsigned char* map = *(const unsigned char* const*)(L->ta + OFF_MAPPED);
    unsigned at = 0, n = 0, changed = 0;
    if (map != r->mmap || mapped_bytes(L->ta) != r->mbytes) return -1;
    if (r->first) {
        if (!mapped_whole(r, map, r->mbytes)) return 0;
        memcpy(r->mshadow, map, r->mbytes);
        return 1;
    }
    if (!memcmp(map, r->mshadow, r->mbytes)) return 1;
    while (at < r->mbytes) {
        unsigned end, len, room;
        if (map[at] == r->mshadow[at]) { at++; continue; }
        /* a run from `at`: on while a changed byte is within MAPPED_GAP of the last */
        for (end = at + 1; end < r->mbytes; end++) {
            unsigned k, more = 0;
            if (map[end] != r->mshadow[end]) continue;
            for (k = end; k < r->mbytes && k < end + MAPPED_GAP; k++)
                if (map[k] != r->mshadow[k]) { more = 1; break; }
            if (!more) break;
        }
        while (at < end) {
            room = REC_MAPPED_RUNS - n;
            if (room < 8 + 4) {
                if (!ring_put(r, RK_MAPPED_RUNS, runs, n, NULL, 0)) return 0;
                n = 0;
                continue;
            }
            len = end - at;
            if (len > room - 8) len = (room - 8) & ~3u;
            memcpy(runs + n, &at, 4);
            memcpy(runs + n + 4, &len, 4);
            memcpy(runs + n + 8, map + at, len);
            n += 8 + len;
            while (n & 3) runs[n++] = 0;
            memcpy(r->mshadow + at, map + at, len);
            changed += len;
            at += len;
        }
    }
    if (n && !ring_put(r, RK_MAPPED_RUNS, runs, n, NULL, 0)) return 0;
    r->mruns++;
    r->mrun_bytes += changed;
    return 1;
}

/* ---- the harness's dump --------------------------------------------------------------------- */

static FILE*         s_dump;
static unsigned      s_dump_raw;
static unsigned long s_dump_crc;
/* A dump armed for GameTime T is taken at the capture point of T, where the world holds iteration T
   (on a puppet host, after the replay's capture-point write), so its GameTime is exact by
   construction, whatever the game's speed or pauses. The table is the game thread's alone: the
   frame visit arms (tagpu_replay.c, the trigger), the capture point takes, the teardown wrap
   clears. It is bounded by its 32 entries, and each name by its 39 characters. */
#define DUMP_ARMED 32                      /* dumps armed at once; the trigger arms one a line */
#define DUMP_NAME  40
static struct {
    char     name[DUMP_NAME];
    unsigned at;                            /* 0: the next capture point                        */
} s_armed[DUMP_ARMED];
static unsigned s_narmed;
static void dump_features(const Level* L, const char* name);

#define DUMP_BODY_MAX 0xFFFFu       /* a record's body length is its header's u16                 */
static unsigned      s_dump_long;           /* this dump's records refused for a longer body   */
static unsigned char s_dump_stage[4 + DUMP_BODY_MAX + 3];

/* One read. The main block, .data, the explored map and the sight counters are read in place, and
   another thread writes .data while the dump runs: the engine's mouse thread 0x4C2990 takes and
   releases the tagged lock 0x52A4E8, with its owner tag 0x52A4EC, about every 33 ms and writes the
   cursor's position (0x51F4B6, 0x51F4BA, 0x51F4D6, 0x51F4DA) under it (DISASSEMBLED
   0x4C2990..0x4C2A74; MEASURED 2026-10-01: each of the 2 of 16 stock-fixture dumps whose sum
   differed from the file sums right with those 8 lock bytes in the lock's other state, and with
   nothing else changed). So each record is copied once
   into s_dump_stage, and the file and the sum both read that copy. The dump runs on the game
   thread alone, which nothing else shares the buffer with. What the copy holds of those bytes is
   whichever value it read, so two dumps of one GameTime can differ there; the recording carries
   none of them (replay-globals.tsv). THE BOUND: the buffer holds the longest body the
   header's u16 can name, and a longer one is refused and counted rather than truncated by the
   cast; the longest written today is an RK_ORDERS, 8 + 512 * 0x58 = 0xB008 bytes. */
static void dump_rec(unsigned kind, const void* pre, unsigned npre, const void* pay, unsigned npay)
{
    RecHdr h;
    unsigned body = npre + npay, n = (4 + body + 3) & ~3u;
    if (body > DUMP_BODY_MAX) {
        s_dump_long++;
        return;
    }
    h.kind = (uint16_t)kind;
    h.len = (uint16_t)body;
    memcpy(s_dump_stage, &h, 4);
    if (npre) memcpy(s_dump_stage + 4, pre, npre);
    if (npay) memcpy(s_dump_stage + 4 + npre, pay, npay);
    memset(s_dump_stage + 4 + body, 0, n - 4 - body);
    fwrite(s_dump_stage, 1, n, s_dump);
    s_dump_crc = Crc32_ComputeBuf(s_dump_crc, s_dump_stage, n);
    s_dump_raw += n;
}

/* a list's records, as olist_put makes them */
static void dump_olist(unsigned slot, unsigned list, const unsigned char* img, unsigned n)
{
    unsigned first = 0;
    do {
        unsigned char pre[8];
        unsigned short s = (unsigned short)slot, len = (unsigned short)n, f = (unsigned short)first;
        unsigned k = n - first < REC_ORDERS_CHUNK ? n - first : REC_ORDERS_CHUNK;
        memcpy(pre, &s, 2);
        pre[2] = (unsigned char)list;
        pre[3] = 0;
        memcpy(pre + 4, &len, 2);
        memcpy(pre + 6, &f, 2);
        dump_rec(RK_ORDERS, pre, 8, img + (size_t)first * REC_ORDER_IMAGE, k * REC_ORDER_IMAGE);
        first += k;
    } while (first < n);
}

/* The globals and the pool as a dump's records, read as the capture reads them. A dump is taken on
   a puppet host too, which records nothing, so the extent is scanned from the pool's first record:
   the same hw the capture's scan reaches, since every record below the peak holds a weapon. */
static void dump_globals_proj(const Level* L, const char* name)
{
    unsigned img[REC_GLOBALS_DWORDS > REC_PROJ_DWORDS ? REC_GLOBALS_DWORDS : REC_PROJ_DWORDS];
    unsigned head[2], i;
    const unsigned char* pool = (const unsigned char*)L->proj;
    globals_image(L->ta, img);
    dump_rec(RK_GLOBALS, NULL, 0, img, REC_GLOBALS_DWORDS * 4);
    if (!pool) {
        tagpu_logf("recorder: dump %s: the level has no projectile pool: the dump leaves it out",
                   name);
        return;
    }
    memcpy(&head[0], L->ta + OFF_NPROJ, 4);
    head[1] = proj_extent(pool, 0, head[0] < TAGPU_LIM_PROJ ? head[0] : TAGPU_LIM_PROJ);
    dump_rec(RK_PROJ_HEAD, head, sizeof head, NULL, 0);
    for (i = 0; i < head[1]; i++) {
        unsigned short pre[2] = { (unsigned short)i, 0 };
        proj_image(L, pool + (size_t)i * REC_PROJ_BYTES, img);
        dump_rec(RK_PROJ, pre, 4, img, REC_PROJ_IMAGE);
    }
}

/* Each player record's sight counters whole, as RK_SIGHT records, for `tarec compare-dumps`: what
   a seek's sight reset and refill rest on, and no other record carries them.
   THE BOUND: read within the block the player init allocated at the size it stored beside it
   (+0x88 at 0x464866, the block at 0x46487B, NULL when the allocation failed).
   THE LIFETIME: the level's teardown 0x464A00 frees every record's block and stores NULL and 0
   in the pointer and the size (0x464A24..0x464A38; DISASSEMBLED). The init, the teardown and the
   dump all run on the game thread, so a record outside the level reads as NULL and 0, and nothing
   is freed under the read. */
static void dump_sight(const char* ta)
{
    unsigned p, off;
    for (p = 0; p < REC_PLAYERS; p++) {
        const char* rec = ta + OFF_PLAYERS + (size_t)p * PL_STRIDE;
        const unsigned char* cnt = *(const unsigned char* const*)(rec + PL_SIGHT);
        unsigned len = *(const unsigned*)(rec + PL_SIGHTLEN);
        if (!cnt) continue;
        for (off = 0; off < len; off += REC_IMAGE_CHUNK) {
            unsigned char pre[8];
            unsigned n = len - off < REC_IMAGE_CHUNK ? len - off : REC_IMAGE_CHUNK;
            pre[0] = (unsigned char)p;
            pre[1] = pre[2] = pre[3] = 0;
            memcpy(pre + 4, &off, 4);
            dump_rec(RK_SIGHT, pre, 8, cnt + off, n);
        }
    }
}

/* The main block and .data whole, as RK_IMAGE records, for `tarec compare --images`: both are
   mapped for the process's life (the head of *The globals and the projectiles*). */
static void dump_images(const char* ta)
{
    unsigned which, off;
    for (which = 0; which < 2; which++) {
        const unsigned char* base = which ? (const unsigned char*)(size_t)DATA_BEGIN
                                          : (const unsigned char*)ta;
        unsigned bytes = which ? REC_DATA_BYTES : REC_MAIN_BYTES;
        for (off = 0; off < bytes; off += REC_IMAGE_CHUNK) {
            unsigned char pre[8];
            unsigned n = bytes - off < REC_IMAGE_CHUNK ? bytes - off : REC_IMAGE_CHUNK;
            pre[0] = (unsigned char)which;
            pre[1] = pre[2] = pre[3] = 0;
            memcpy(pre + 4, &off, 4);
            dump_rec(RK_IMAGE, pre, 8, base + off, n);
        }
    }
}

/* The live world's images as a stored RB_SNAPSHOT: what the recording must read back to at this
   GameTime, and the two images whole after it. Before it, a stored RB_HEADER with the format, the
   build, the globals list's hash and the tables, so that a dump taken by another build than the
   recording's (a playing, D8) is read with its own table names. Written on the game thread, which
   is tooling's price: a dump is the harness's. */
static void dump_counts(void);

static int dump_write(const Level* L, int tick, const char* name, int frame)
{
    char path[MAX_PATH];
    RecBlock b;
    unsigned slot, i, img[IMG_DWORDS], n = tagpu_simreg_count();
    static unsigned char rb[SPAN_MAX];
    static unsigned char hb[HEADER_MAX];
    Tlv t;
    long at;
    const SimDesc* wide = table_too_wide();
    unsigned char* lists;
    if (wide) {
        tagpu_logf("recorder: dump %s refused: %s has rows past %u bytes", name, wide->name,
                   SPAN_MAX);
        return 0;
    }
    t.b = hb;
    t.n = 0;
    t.cap = sizeof hb;
    t.full = 0;
    tlv_u32(&t, RT_FORMAT, REC_FORMAT);
    tlv(&t, RT_BUILD, GIT_COMMIT, (unsigned)strlen(GIT_COMMIT));
    tlv(&t, RT_GLOBALS, TAGPU_REPLAY_GLOBALS_SHA256, (unsigned)strlen(TAGPU_REPLAY_GLOBALS_SHA256));
    if (frame >= 0) tlv_u32(&t, RT_FRAME, (unsigned)frame);
    {
        unsigned v[2] = { s_dllBase, s_dllSize };
        tlv(&t, RT_DLL, v, 8);
    }
    for (i = 0; i < n; i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        if (d && table_recorded(d)) {
            table_entry(&t, i, d, tagpu_simreg_layout_hash(d));
            table_fields(&t, i, d);
        }
    }
    if (t.full) {
        tagpu_logf("recorder: dump %s refused: its header does not fit in %u bytes", name, t.cap);
        return 0;
    }
    lists = (unsigned char*)VirtualAlloc(NULL, ORD_SCRATCH, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
    if (!lists) { tagpu_logf("recorder: dump %s: no memory for the order lists", name); return 0; }
    CreateDirectoryA("replays", NULL);
    snprintf(path, sizeof path, "replays\\dump-%s.bin", name);
    s_dump = fopen(path, "wb");
    if (!s_dump) {
        tagpu_logf("recorder: dump %s: cannot write %s", name, path);
        VirtualFree(lists, 0, MEM_RELEASE);
        return 0;
    }
    fwrite(REC_MAGIC, 1, 8, s_dump);
    memset(&b, 0, sizeof b);
    b.magic = REC_BLOCK_MAGIC;
    b.kind = RB_HEADER;
    b.flags = RBF_STORED;
    b.first = b.last = (uint32_t)tick;
    b.raw = b.packed = t.n;
    b.crc_body = (uint32_t)Crc32_ComputeBuf(0, hb, t.n);
    b.crc_head = (uint32_t)Crc32_ComputeBuf(0, &b, 28);
    fwrite(&b, sizeof b, 1, s_dump);
    fwrite(hb, 1, t.n, s_dump);
    at = ftell(s_dump);
    memset(&b, 0, sizeof b);
    fwrite(&b, sizeof b, 1, s_dump);                  /* rewritten once the body is known */
    s_dump_raw = 0;
    s_dump_crc = 0;
    s_dump_long = 0;
    dump_rec(RK_TICK, &tick, 4, NULL, 0);
    for (slot = 1; slot < L->nslots; slot++) {
        unsigned short pre[2] = { (unsigned short)slot, 0 };
        unsigned list, k;
        if (!unit_live(L, slot)) continue;
        unit_image(L, slot, img);
        dump_rec(RK_UNIT, pre, 4, img, REC_UNIT_IMAGE);
        for (list = 0; list < 2; list++) {
            int why = olist_read(L, slot, list, lists, &k, NULL);
            if (why != OL_OK) {
                tagpu_logf("recorder: dump %s: slot %u's %s list cannot be read (%d): the dump "
                           "leaves it out", name, slot, list ? "sub" : "main", why);
                continue;
            }
            if (k) dump_olist(slot, list, lists, k);
        }
    }
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned short pre[2] = { (unsigned short)i, 0 };
        unsigned char v[4];
        player_image(L, i, img);
        dump_rec(RK_PLAYER, pre, 4, img, REC_PLAYER_DWORDS * 4);
        v[0] = (unsigned char)i;
        v[1] = (unsigned char)L->ta[OFF_PLAYERS + i * PL_STRIDE + PL_CONTROLLER];
        v[2] = v[3] = 0;
        dump_rec(RK_CONTROLLER, v, 4, NULL, 0);
    }
    {
        unsigned char v[4];
        v[0] = (unsigned char)L->ta[OFF_CONTROLLED];
        v[1] = (unsigned char)L->ta[OFF_VIEWED];
        v[2] = v[3] = 0;
        dump_rec(RK_VIEWER, v, 4, NULL, 0);
    }
    for (i = 0; i < n; i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        unsigned bytes, off;
        unsigned char pre[8];
        unsigned short ix = (unsigned short)i, z = 0;
        const unsigned char* base;
        if (!d || !table_recorded(d)) continue;
        base = table_rows(d, &bytes);
        if ((d->flags & SIMD_HEAP) && bytes > heap_limit(base))
            bytes = heap_limit(base) - heap_limit(base) % d->stride;
        memcpy(pre, &ix, 2);
        memcpy(pre + 2, &z, 2);
        memcpy(pre + 4, &bytes, 4);
        dump_rec(RK_TABLE_SIZE, pre, 8, NULL, 0);
        for (off = 0; off < bytes; off += d->stride) {
            row_image(L, d, base + off, rb);
            memcpy(pre + 4, &off, 4);
            dump_rec(RK_TABLE_SPAN, pre, 8, rb, d->stride);
        }
    }
    dump_features(L, name);
    dump_globals_proj(L, name);
    {
        const unsigned char* map = *(const unsigned char* const*)(L->ta + OFF_MAPPED);
        if (map && mapped_bytes(L->ta)) mapped_whole(NULL, map, mapped_bytes(L->ta));
        else tagpu_logf("recorder: dump %s: the level has no explored map: the dump leaves it out",
                        name);
    }
    dump_sight(L->ta);
    dump_images(L->ta);
    dump_counts();
    b.magic = REC_BLOCK_MAGIC;
    b.kind = RB_SNAPSHOT;
    b.flags = RBF_STORED;
    b.first = b.last = (uint32_t)tick;
    b.raw = b.packed = s_dump_raw;
    b.crc_body = (uint32_t)s_dump_crc;
    b.crc_head = (uint32_t)Crc32_ComputeBuf(0, &b, 28);
    fseek(s_dump, at, SEEK_SET);
    fwrite(&b, sizeof b, 1, s_dump);
    fclose(s_dump);
    s_dump = NULL;
    VirtualFree(lists, 0, MEM_RELEASE);
    if (frame >= 0)
        tagpu_logf("recorder: frame dump %s at GameTime %d (iteration %d) -> %s (%u bytes of "
                   "records); %u records with a body past 0xFFFF bytes not written", name, frame,
                   tick, path, s_dump_raw, s_dump_long);
    else
        tagpu_logf("recorder: dump %s at GameTime %d -> %s (%u bytes of records); %u records with "
                   "a body past 0xFFFF bytes not written", name, tick, path, s_dump_raw,
                   s_dump_long);
    return 1;
}

/* a dump's name: 1 to DUMP_NAME-1 letters, digits, - and _ (its file is replays\dump-<name>.bin);
   its length, or 0 with a line */
static size_t dump_name(const char* name)
{
    size_t i, n = bounded_len(name, DUMP_NAME);
    if (!n || n == DUMP_NAME) {
        tagpu_logf("recorder: dump: a name of 1 to %u characters, please", DUMP_NAME - 1);
        return 0;
    }
    for (i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_')) {
            tagpu_logf("recorder: dump: the name %.*s is not letters, digits, - and _", (int)n, name);
            return 0;
        }
    return n;
}

int tagpu_rec_dump_now(const char* name)
{
    char* ta = ta_main();
    Level L;
    int now;
    if (!dump_name(name)) return 0;
    if (!ta || !level_read(ta, &L)) {
        tagpu_logf("recorder: frame dump %s refused: no level is in play", name);
        return 0;
    }
    masks_init();
    now = *(const int*)(ta + OFF_GAMETIME);
    return dump_write(&L, now + 1, name, now);
}

int tagpu_rec_dump_arm(const char* name, unsigned at)
{
    const char* ta = ta_main();
    size_t i, n = dump_name(name);
    unsigned now;
    if (!n) return 0;
    if (s_narmed == DUMP_ARMED) {
        tagpu_logf("recorder: dump %s refused: %u dumps are armed already", name, DUMP_ARMED);
        return 0;
    }
    for (i = 0; i < s_narmed; i++)
        if (!strcmp(s_armed[i].name, name)) {
            /* two dumps of one name would write one file */
            tagpu_logf("recorder: dump %s refused: a dump of that name is armed already", name);
            return 0;
        }
    if (at) {
        /* the next capture point is GameTime now + 1 */
        now = ta ? *(const unsigned*)(ta + OFF_GAMETIME) : 0;
        if (!ta || at <= now) {
            tagpu_logf("recorder: dump %s at GameTime %u refused: the level is at GameTime %u",
                       name, at, now);
            return 0;
        }
    }
    memcpy(s_armed[s_narmed].name, name, n);
    s_armed[s_narmed].name[n] = 0;
    s_armed[s_narmed].at = at;
    s_narmed++;
    if (at) tagpu_logf("recorder: dump %s armed for GameTime %u", name, at);
    return 1;
}

/* the armed dumps at the capture point of `tick`, in arm order: an untimed one or one for this
   GameTime taken, one for an earlier GameTime dropped (a forward seek passed it), the rest kept */
static void dumps_take(const Level* L, int tick)
{
    unsigned i, k = 0;
    for (i = 0; i < s_narmed; i++) {
        if (!s_armed[i].at || s_armed[i].at == (unsigned)tick) {
            dump_write(L, tick, s_armed[i].name, -1);
        } else if (s_armed[i].at < (unsigned)tick) {
            tagpu_logf("recorder: dump %s for GameTime %u not taken: the level is at GameTime %d",
                       s_armed[i].name, s_armed[i].at, tick);
        } else {
            s_armed[k++] = s_armed[i];
        }
    }
    s_narmed = k;
}

/* ---- the events ----------------------------------------------------------------------------- */

/* THE EVENTS. What the engine does in a step that the images after it cannot say -- a unit made or
   destroyed, a shot, a script started, a nano spray -- recorded where the engine does it, in the
   order it does it, after the images of the iteration before (tagpu_recfmt.h). Each hook observes
   an engine function, or a call of one, and changes nothing it does, with one exception: the
   network tests of the five fire functions, released while a skirmish is recorded so that they
   build their 0x0D. Between the test and the send each block writes only the packet on its own
   stack, and calls only the send -- which wpn_tx_fired skips when there is no network -- and,
   for the meteor, 0x44FDB0, which reads a netid (demo-recorder-s2-events.md §1.4). Four of them
   also run the extra weapons' slot-index splice, which writes no simulation state
   (tagpu_weapons.c, cb_slot_index, says why).

   Only the recording's game thread records (s_tid). An event that does not fit in the ring marks
   the step's events incomplete: the next capture ends the recording (REC_END_RING) without
   publishing them. The pointers a hook reads are the engine's own arguments or registers at the
   site, read no further than the engine reads them; a unit is named only once its pointer is a
   slot of the array by arithmetic (id_of), and a unit's own fields are read only after that. */

static unsigned char s_sites;           /* every hook is in: tagpu_rec_init                  */

/* the recording, when this is its game thread; NULL otherwise, a refusal counted */
static Rec* ev_rec(void)
{
    DWORD t = s_tid;
    if (!t) return NULL;
    if (t != GetCurrentThreadId()) { InterlockedIncrement(&s_offthread); return NULL; }
    return s_rec;
}

static void ev_put(Rec* r, unsigned kind, const void* b, unsigned n)
{
    if (r->ev_full || ring_put(r, kind, b, n, NULL, 0)) return;
    r->ev_full = 1;
    tagpu_logf("recorder: an event did not fit in the ring after GameTime %d: the recording ends "
               "at the next iteration", r->tick);
}

/* a unit's ID, when `p` is a slot of the array; 0 otherwise (NULL, or counted as a bad ID) */
static unsigned ev_unit(const Rec* r, const char* p)
{
    unsigned id = id_of(&r->L, RL_T_UNIT, p);
    return id == REC_ID_BAD ? 0 : id;
}

/* the entry's registers, as the observer's pushad left them below the entry's esp */
#define REG_EDI(E) (*(const unsigned*)((const unsigned char*)(E) - 0x24))
#define REG_ESI(E) (*(const unsigned*)((const unsigned char*)(E) - 0x20))
#define REG_ECX(E) (*(const unsigned*)((const unsigned char*)(E) - 0x0C))
#define REG_EAX(E) (*(const unsigned*)((const unsigned char*)(E) - 0x08))
#define ARG(E, off) (*(const unsigned*)((const unsigned char*)(E) + (off)))
#define PTR(v) ((const char*)(size_t)(v))

/* THE COUNTS, for the harness's compare-counts (demo-recorder-completeness.md, *Also asserted*):
   how often each engine root, sink and observed site ran in a level that records or plays a
   recording, keyed (site, caller, aux), so that a live game and its playings can be compared call
   for call. A site is its VA (a root's or an observer's entry, a COB write's return address); the
   caller the return address, or 0 where the site is not a function's entry; aux what tells calls
   of one site apart (a sink's pool, a script's name hashed). A caller inside this DLL is
   (offset | REC_COUNT_DLL), which does not depend on where the DLL loaded.

   Counted on the level's game thread only (s_cnt.tid, set at its first capture point), never
   while a seek rebuilds (tagpu_puppet_rebuilding: its kills and creates are not the recording's),
   and reset at each level's first capture point. Open addressing in CNT_ROWS rows; a key that
   finds none is counted in `overflow`, which fails the comparison by name. Presentation state:
   nothing the simulation reads. */
#define CNT_ROWS  4096
#define CNT_NAMES 512
typedef struct { unsigned va, caller, aux, n; } CntRow;
static struct {
    DWORD    tid;
    Level    L;                         /* the level's unit array, for bounding a unit        */
    unsigned rows, overflow;            /* rows used; keys that found none                    */
    CntRow   row[CNT_ROWS];
    unsigned nnames;
    struct { unsigned aux; char name[REC_COUNT_NAME]; } name[CNT_NAMES];
} s_cnt;
static unsigned s_cntSinks;             /* the sinks whose observer is in, bit k of CNT_SINKS  */

static unsigned cnt_caller(unsigned ret)
{
    return ret - s_dllBase < s_dllSize ? (ret - s_dllBase) | REC_COUNT_DLL : ret;
}

/* 1 when this call is counted: the level's game thread, no seek rebuilding */
static int cnt_on(void)
{
    return s_cnt.tid && s_cnt.tid == GetCurrentThreadId() && !tagpu_puppet_rebuilding();
}

static void cnt_add(unsigned va, unsigned caller, unsigned aux)
{
    unsigned h = (va * 0x9E3779B1u) ^ (caller * 0x85EBCA77u) ^ (aux * 0xC2B2AE3Du), k;
    for (k = 0; k < CNT_ROWS; k++) {
        CntRow* c = &s_cnt.row[(h + k) & (CNT_ROWS - 1)];
        if (!c->n) {
            c->va = va;
            c->caller = caller;
            c->aux = aux;
            c->n = 1;
            s_cnt.rows++;
            return;
        }
        if (c->va == va && c->caller == caller && c->aux == aux) {
            c->n++;
            return;
        }
    }
    s_cnt.overflow++;
}

static void cnt(unsigned va, unsigned caller, unsigned aux)
{
    if (cnt_on()) cnt_add(va, caller, aux);
}

/* the counts' state at a level's first capture point: counting when it records or plays */
static void cnt_start(const Level* L, int counting)
{
    memset(&s_cnt, 0, sizeof s_cnt);
    if (!counting) return;
    s_cnt.L = *L;
    s_cnt.tid = GetCurrentThreadId();
}


/* The create 0x485F50's success exit 0x4861B3 (`mov eax,esi`): esi is the unit it made, in the
   slot it actually took -- B4's hit_take may move a first-free create past its first choice. The
   function has pushed five registers, so its arguments start at +0x18; the first, the owner, is
   overwritten in the body (0x485F9C), so the owner is the unit's own +0x96. The water state +0x10A
   is still the slot's here: nothing in 0x485A40..0x4861BA stores to it, and 0x43DB50 alone writes
   it, from the unit tick (0x48AFAA) and the 0x2C receiver (0x48BA46) through 0x43DD20. */
static int __cdecl ev_create(void* E)
{
    Rec* r;
    const char* unit;
    unsigned id, owner, v;
    unsigned char b[28];
    cnt(0x004861B3u, 0, 0);
    if (!(r = ev_rec())) return 0;
    unit = PTR(REG_ESI(E));
    if (!(id = ev_unit(r, unit))) return 0;
    owner = id_of(&r->L, RL_T_PLAYER, *(const char* const*)(unit + U_OWNER));
    memcpy(b, &id, 4);
    v = ARG(E, 0x1C) & 0xFFFF;                        /* the type: `mov dx,[esp+0x1C]`, 0x486079 */
    memcpy(b + 4, &v, 2);
    b[6] = (unsigned char)(owner && owner != REC_ID_BAD ? owner - 1 : 0xFF);
    b[7] = (unsigned char)(ARG(E, 0x30) & 3);         /* the create keeps bits 0-1 (0x4860F1)    */
    v = ARG(E, 0x2C);
    memcpy(b + 8, &v, 4);
    memcpy(b + 12, (const unsigned char*)E + 0x20, 12);   /* the position, three dwords by value */
    b[24] = (unsigned char)unit[U_WATER];
    b[25] = b[26] = b[27] = 0;
    ev_put(r, RK_EV_CREATE, b, sizeof b);
    return 0;
}

/* the player a netid names, by 0x44FE40's rule: the first seat with a controller whose netid it
   is, and 10 for none */
static unsigned player_of_netid(const char* ta, unsigned netid)
{
    unsigned i;
    if (netid == 0xFFFFFFFFu) return 10;
    for (i = 0; i < REC_PLAYERS; i++) {
        const char* rec = ta + OFF_PLAYERS + (size_t)i * PL_STRIDE;
        if (rec[PL_CONTROLLER] && *(const unsigned*)(rec + 4) == netid) return i;
    }
    return 10;
}

/* The destructor 0x4866D0(rec, mode)'s entry: the victim's u16 at rec+1, the killer's at +7, the
   killer's netid at +3, the severity at +9 and the death's kind at +0xA, all of which the
   destructor reads next. A call on a slot that is not a live unit's is inert -- the alive test at
   0x486706, and B3's bound at 0x4866E5 for 0 and past the array -- so it is not recorded. The
   killer is named as the engine resolves it: 0 for none (0x48675A), and B3's bound makes one past
   the array none too. The image is the victim's as the destructor finds it: the reaper runs after
   the unit's own movement step, so the iteration's image is a step early for the wreck and the
   blast. */
static int death_room(Rec* r);
static int death_enter(Rec* r, void* E, unsigned id, unsigned owner);

#define OFF_SIDES      0x37F39      /* u32: the sides this game loaded; the lobby clamps a side at
                                       or above it (0x447EC0..0x447ECE)                          */
#define OFF_SIDE_TAB   0x37F5F      /* each side's commander name: +0x22 into its record, which the
                                       loader 0x431A60 lays out from main+0x37F3D (0x431AA6)     */
#define SIDE_STRIDE    0x232
#define SIDE_ROOM      5u           /* the engine's own sides: its free loop stops at five records
                                       (0x431A20, cmp edi,0xAFA), which end at main+0x38A37; the
                                       loader has no cap of its own on SIDE%d                    */

/* The index of the player record `owner` points at, or REC_PLAYERS when it is none of them. */
static unsigned owner_index(const char* ta, const char* owner)
{
    const char* recs = ta + OFF_PLAYERS;
    size_t d = (size_t)(owner - recs);
    if (owner < recs || d % PL_STRIDE || d / PL_STRIDE >= REC_PLAYERS) return REC_PLAYERS;
    return (unsigned)(d / PL_STRIDE);
}

/* Send_UnitDeath's commander test (0x4864CB..0x48650F): the unit's UnitName against its owner's
   side's commander name, case-insensitively. The def is bounded by the engine's own count
   (UNITINFOCount), the side by the count of sides loaded and by the table's room, so every byte
   read lies in the defs or the main block. The engine's _stricmp (0x4F8A70) and this compare of
   0x20 agree: the loader stores the name within 0x20 bytes, its NUL included (0x431B6D). */
static int is_commander(const Level* L, const char* unit, unsigned owner)
{
    const char* def = *(const char* const*)(unit + U_TYPE);
    const char* rec = L->ta + OFF_PLAYERS + (size_t)owner * PL_STRIDE;
    const unsigned char* info = *(const unsigned char* const*)(rec + PL_INFO);
    size_t d = (size_t)(def - L->defs);
    unsigned side;
    if (!info || def < L->defs || d % UD_STRIDE || d / UD_STRIDE >= L->ndefs) return 0;
    side = info[0x95];
    if (side >= *(const unsigned*)(L->ta + OFF_SIDES) || side >= SIDE_ROOM) return 0;
    return !_strnicmp(def + UD_NAME, L->ta + OFF_SIDE_TAB + (size_t)side * SIDE_STRIDE, 0x20);
}

static int __cdecl ev_death(void* E)
{
    Rec* r;
    const unsigned char* msg = (const unsigned char*)PTR(ARG(E, 4));
    unsigned short slot, ks;
    unsigned id, kid;
    unsigned char b[12 + REC_UNIT_IMAGE];
    memcpy(&slot, msg + 1, 2);
    /* the destructor's own alive test (0x486706) on a slot bounded first */
    if (cnt_on() && slot && slot < s_cnt.L.nslots && unit_live(&s_cnt.L, slot))
        cnt_add(0x004866D0u, ARG(E, 0), 0);
    if (!(r = ev_rec())) return 0;
    if (!slot || slot >= r->L.nslots || !unit_live(&r->L, slot)) return 0;
    if (!death_room(r)) return 0;
    memcpy(&ks, msg + 7, 2);
    id = slot + 1u;
    kid = ks && ks < r->L.nslots ? ks + 1u : 0;
    memcpy(b, &id, 4);
    memcpy(b + 4, &kid, 4);
    b[8] = (unsigned char)player_of_netid(r->L.ta, *(const unsigned*)(msg + 3));
    b[9] = msg[9];
    b[10] = msg[0xA];
    b[11] = (unsigned char)tagpu_tx_carried(r->L.begin + (size_t)slot * REC_UNIT_BYTES);
    unit_image(&r->L, slot, (unsigned*)(b + 12));
    ev_put(r, RK_EV_DEATH, b, sizeof b);
    {
        const char* unit = r->L.begin + (size_t)slot * REC_UNIT_BYTES;
        unsigned owner = owner_index(r->L.ta, *(const char* const*)(unit + U_OWNER));
        if (owner < REC_PLAYERS && is_commander(&r->L, unit, owner))
            r->cmdr[owner] = *(const unsigned*)(r->L.ta + OFF_GAMETIME);
        return death_enter(r, E, id, owner);
    }
}

/* a unit slot of a packet as its ID, in a u16: 0 stays 0, and a slot past the array is 0xFFFF */
static void shot_id(const Rec* r, unsigned char* at)
{
    unsigned short v;
    memcpy(&v, at, 2);
    if (v) {
        if (v < r->L.nslots) v = (unsigned short)(v + 1);
        else { v = 0xFFFF; s_bad++; }
    }
    memcpy(at, &v, 2);
}

void tagpu_rec_shot(const unsigned char* pkt, int meteor)
{
    Rec* r = ev_rec();
    unsigned char b[36];
    if (!r) return;
    memcpy(b, pkt, sizeof b);
    if (meteor) {
        /* the stack's leftovers; only the raised build carries weapon bits 8-11 at +0x23 */
        memset(b + 0x1A, 0, 9);
        b[0x23] = (unsigned char)(TAGPU_LIM_WEAPONS > 256 ? b[0x23] & 0xF0 : 0);
    } else {
        b[0x1A] &= 1;                  /* the intercepting weapon's bit; the rest is the stack's */
        shot_id(r, b + 0x1F);
        shot_id(r, b + 0x21);
    }
    ev_put(r, RK_EV_SHOT, b, sizeof b);
}

void tagpu_rec_intercept(const unsigned char* point, unsigned weapon)
{
    Rec* r = ev_rec();
    unsigned char b[16];
    unsigned short w = (unsigned short)weapon, z = 0;
    if (!r) return;
    memcpy(b, point, 12);
    memcpy(b + 12, &w, 2);
    memcpy(b + 14, &z, 2);
    ev_put(r, RK_EV_INTERCEPT, b, sizeof b);
}

/* A broadcast start: the unit, the script's index and arguments as the 0x10 carries them -- the
   index an i16, argc a byte, four arguments. */
static void ev_start(const char* unit, unsigned idx, unsigned argc, const unsigned char* args)
{
    Rec* r = ev_rec();
    unsigned char b[28];
    unsigned id;
    int v;
    if (!r || !(id = ev_unit(r, unit))) return;
    memcpy(b, &id, 4);
    v = (short)(idx & 0xFFFF);
    memcpy(b + 4, &v, 4);
    v = (int)(argc & 0xFF);
    memcpy(b + 8, &v, 4);
    if (args) memcpy(b + 12, args, 16);
    else memset(b + 12, 0, 16);
    ev_put(r, RK_EV_START, b, sizeof b);
}

/* 0x456190(unit, idx): no arguments */
static int __cdecl ev_start_idx(void* E)
{
    cnt(0x00456190u, ARG(E, 0), 0);
    ev_start(PTR(ARG(E, 4)), ARG(E, 8), 0, NULL);
    return 0;
}

/* 0x456290(unit, idx, argc, a0..a3) */
static int __cdecl ev_start_args(void* E)
{
    cnt(0x00456290u, ARG(E, 0), 0);
    ev_start(PTR(ARG(E, 4)), ARG(E, 8), ARG(E, 0xC), (const unsigned char*)E + 0x10);
    return 0;
}

/* 0x456218, inside 0x456200(unit, name, argc, a0..a3) once 0x4B07C0 has turned the name into
   the index, which it broadcasts: esi the unit, eax the index, and the arguments 0x1C further up
   (`sub esp,0x18; push esi`) */
static int __cdecl ev_start_named_bc(void* E)
{
    cnt(0x00456218u, 0, 0);
    ev_start(PTR(REG_ESI(E)), REG_EAX(E), ARG(E, 0x28), (const unsigned char*)E + 0x2C);
    return 0;
}

/* The build-done 0x41B8D0(builder, built): its own tests make a call on a dead or absent unit
   inert, so every call is recorded as made. */
static int __cdecl ev_built(void* E)
{
    Rec* r;
    unsigned b[2];
    cnt(0x0041B8D0u, ARG(E, 0), 0);
    if (!(r = ev_rec())) return 0;
    b[0] = id_of(&r->L, RL_T_UNIT, PTR(ARG(E, 4)));
    b[1] = id_of(&r->L, RL_T_UNIT, PTR(ARG(E, 8)));
    if (b[0] == REC_ID_BAD || b[1] == REC_ID_BAD) return 0;
    ev_put(r, RK_EV_BUILT, b, sizeof b);
    return 0;
}

/* The state setter 0x48B090(mask, value), ecx the unit: the byte +0x10E as it will be, by the
   setter's own rule (0x48B09A..0x48B0D4), when it changes -- the setter returns at once when it
   does not (0x48B0DE). */
static int __cdecl ev_state(void* E)
{
    Rec* r;
    const char* unit = PTR(REG_ECX(E));
    unsigned id;
    unsigned char was, now, b[8];
    /* only a call that changes the byte, by the setter's own test (0x48B09A..0x48B0DE): a replayed
       root's nested call and its replayed event both reach it, and the second returns early */
    if (cnt_on() && id_of(&s_cnt.L, RL_T_UNIT, unit) - 1u < s_cnt.L.nslots) {
        was = (unsigned char)unit[U_STATEBYTE];
        now = ARG(E, 8) ? (unsigned char)(was | ARG(E, 4)) : (unsigned char)(was & ~ARG(E, 4));
        if (now != was) cnt_add(0x0048B090u, ARG(E, 0), 0);
    }
    if (!(r = ev_rec())) return 0;
    if (!(id = ev_unit(r, unit))) return 0;
    was = (unsigned char)unit[U_STATEBYTE];
    now = ARG(E, 8) ? (unsigned char)(was | ARG(E, 4)) : (unsigned char)(was & ~ARG(E, 4));
    if (now == was) return 0;
    memcpy(b, &id, 4);
    b[4] = now;
    b[5] = b[6] = b[7] = 0;
    ev_put(r, RK_EV_STATE, b, sizeof b);
    return 0;
}

/* The two replies 0x47F780 and 0x47F850(unit, kind, text), before their viewer test: every
   player's. The text is the caller's string, NULL for the kind's own; it is copied no further than
   its end, as far as the engine's own copy reads it. */
static void ev_reply(void* E, unsigned root)
{
    Rec* r = ev_rec();
    const char* text;
    unsigned id, kind, n = 0;
    unsigned char b[12 + REC_TEXT_MAX];
    if (!r || !(id = ev_unit(r, PTR(ARG(E, 4))))) return;
    kind = ARG(E, 8);
    text = PTR(ARG(E, 0xC));
    memcpy(b, &id, 4);
    memcpy(b + 4, &kind, 4);
    b[8] = (unsigned char)root;
    b[9] = (unsigned char)(text != NULL);
    b[10] = b[11] = 0;
    if (text) {
        n = (unsigned)bounded_len(text, REC_TEXT_MAX);
        memcpy(b + 12, text, n);
    }
    ev_put(r, RK_EV_REPLY, b, 12 + n);
}

static int __cdecl ev_reply0(void* E) { cnt(0x0047F780u, ARG(E, 0), 0); ev_reply(E, 0); return 0; }
static int __cdecl ev_reply1(void* E) { cnt(0x0047F850u, ARG(E, 0), 0); ev_reply(E, 1); return 0; }

/* A start of a script by name that only the owner's machine makes: the unit, the start's third
   argument (nonzero runs it at once, 0x4B0B84), its arguments and the name. */
static void ev_named(Rec* r, unsigned id, const char* name, unsigned now, unsigned argc,
                     const unsigned char* args)
{
    unsigned char b[24 + REC_NAME_MAX];
    unsigned n = (unsigned)bounded_len(name, REC_NAME_MAX);
    memcpy(b, &id, 4);
    b[4] = (unsigned char)argc;
    b[5] = (unsigned char)(now != 0);
    b[6] = b[7] = 0;
    if (args) memcpy(b + 8, args, 16);
    else memset(b + 8, 0, 16);
    memcpy(b + 24, name, n);
    ev_put(r, RK_EV_START_NAMED, b, 24 + n);
}

void tagpu_rec_start_named(const char* unit, const char* name, int now, int argc, int a0)
{
    Rec* r = ev_rec();
    int a[4] = { 0, 0, 0, 0 };
    unsigned id;
    if (!r || !(id = ev_unit(r, unit))) return;
    a[0] = a0;
    ev_named(r, id, name, (unsigned)now, (unsigned)argc, (const unsigned char*)a);
}

/* THE OWNER-ONLY STARTS. Each site is a `call` of 0x4B0A70(name, 0, now, argc, a0..a3) or of
   0x4B0940(name, 0, now), both thiscall on the unit's COB object, and each loads that ecx from the
   unit's +0x9A just before, `mov ecx,[reg+0x9A]` at `mov`, with `reg` holding the unit until the
   call (DISASSEMBLED). The stub runs ev_start_site with the call's esp -- the return address, then
   the arguments -- and the registers under it, then jumps to the function as the call would. */
typedef struct StartSite {
    unsigned call, mov;       /* the call, and the mov that loads its ecx                         */
    unsigned target;          /* 0x4B0A70, whose arguments are recorded, or 0x4B0940              */
    unsigned char reg;        /* the unit's register: pushad's order, EDI 0 .. EAX 7             */
    const char* what;
} StartSite;
enum { R_EDI, R_ESI, R_EBP, R_ESP, R_EBX, R_EDX, R_ECX, R_EAX };
static const StartSite START_SITES[] = {
    { 0x00489F43u, 0x00489F2Cu, 0x004B0A70u, R_ESI, "HitByWeapon" },
    { 0x00489F8Eu, 0x00489F76u, 0x004B0A70u, R_ESI, "TakeDamage" },
    { 0x00406834u, 0x00406823u, 0x004B0A70u, R_EDI, "TransportPickup" },
    { 0x004069BFu, 0x004069A6u, 0x004B0A70u, R_EDI, "TransportDrop" },
    { 0x00489898u, 0x00489880u, 0x004B0A70u, R_EDI, "TargetCleared, 0x489800" },
    { 0x00489948u, 0x00489930u, 0x004B0A70u, R_EDI, "TargetCleared, 0x4898B0" },
    { 0x0048A149u, 0x0048A131u, 0x004B0A70u, R_EDI, "TargetCleared, 0x48A0F0" },
    { 0x0048A2E0u, 0x0048A2C8u, 0x004B0A70u, R_EDI, "TargetCleared, 0x48A1E0" },
    { 0x0040F433u, 0x0040F424u, 0x004B0940u, R_EBP, "EndTransport" },
    { 0x0041148Du, 0x00411480u, 0x004B0940u, R_ESI, "EndTransport" },
    { 0x00411794u, 0x00411785u, 0x004B0940u, R_EBX, "EndTransport" },
    { 0x00411DA1u, 0x00411D92u, 0x004B0940u, R_EBX, "EndTransport" },
    { 0x00411E2Bu, 0x00411E1Eu, 0x004B0940u, R_EBP, "EndTransport" },
};
#define START_SITE_COUNT (sizeof START_SITES / sizeof START_SITES[0])

static int __cdecl ev_start_site(void* E, unsigned site)
{
    const StartSite* st = &START_SITES[site];
    Rec* r;
    const char* unit;
    unsigned id;
    cnt(st->call, 0, 0);
    if (!(r = ev_rec())) return 0;
    unit = PTR(((const unsigned*)((const unsigned char*)E - 0x24))[st->reg]);
    if (!(id = ev_unit(r, unit))) return 0;
    if (*(const unsigned*)(unit + U_COB) != REG_ECX(E)) {
        s_bad++;                                      /* not the unit the disassembly says */
        return 0;
    }
    ev_named(r, id, PTR(ARG(E, 4)), ARG(E, 0xC), st->target == 0x004B0A70u ? ARG(E, 0x10) : 0,
             st->target == 0x004B0A70u ? (const unsigned char*)E + 0x14 : NULL);
    return 0;
}

/* THE NANO SPRAY. Its two emitters 0x4720D0(point, box, layer) and 0x472200(box, point, layer)
   are called only from fifteen order handlers, and each handler asks QueryNanoPiece 0x43E400(unit,
   out) for the point first, into a buffer on its own stack that it then passes to the emitter as
   the point (the engine map, *The nano spray*). A spray is credited to the unit of the last query
   whose out buffer is its point; the handler calls, the only way into an emitter, forget the last
   query, so a query cannot credit a spray of another handler's call. A spray with no such query is
   recorded with ID 0 and counted. */
static const char* s_nano_unit;
static const void* s_nano_out;

static int __cdecl ev_nano_query(void* E)
{
    cnt(0x0043E400u, ARG(E, 0), 0);
    if (ev_rec()) {
        s_nano_unit = PTR(ARG(E, 4));
        s_nano_out = PTR(ARG(E, 8));
    }
    return 0;
}

/* the three handler calls, 0x43A21A, 0x43B87C and 0x43BB21 */
static int __cdecl ev_nano_handler(void* E)
{
    (void)E;
    if (ev_rec()) s_nano_unit = NULL, s_nano_out = NULL;
    return 0;
}
static int __cdecl ev_nano_h0(void* E) { cnt(0x0043A216u, 0, 0); return ev_nano_handler(E); }
static int __cdecl ev_nano_h1(void* E) { cnt(0x0043B878u, 0, 0); return ev_nano_handler(E); }
static int __cdecl ev_nano_h2(void* E) { cnt(0x0043BB1Du, 0, 0); return ev_nano_handler(E); }

static void ev_spray(void* E, unsigned reverse)
{
    Rec* r = ev_rec();
    const void* point = PTR(ARG(E, reverse ? 8 : 4));
    const void* box = PTR(ARG(E, reverse ? 4 : 8));
    unsigned id = 0, layer = ARG(E, 0xC);
    unsigned char b[48];
    if (!r) return;
    if (s_nano_unit && point == s_nano_out) id = ev_unit(r, s_nano_unit);
    if (!id) r->uncredited++;
    memcpy(b, &id, 4);
    memcpy(b + 4, &reverse, 4);
    memcpy(b + 8, point, 12);
    memcpy(b + 20, box, 24);
    memcpy(b + 44, &layer, 4);
    ev_put(r, RK_EV_SPRAY, b, sizeof b);
}

static int __cdecl ev_spray0(void* E) { cnt(0x004720D0u, ARG(E, 0), 0); ev_spray(E, 0); return 0; }
static int __cdecl ev_spray1(void* E) { cnt(0x00472200u, ARG(E, 0), 0); ev_spray(E, 1); return 0; }

/* ---- the features --------------------------------------------------------------------------- */

/* THE FEATURES: the feature grid, the wreck pool, the heads of the pool's three lists and the
   reproduction cursor (demo-recorder-skirmish-slice.md, S3a).

   WHAT IS READ, AND WHY IN BOUNDS. The grid [main+0x14287] is W x H cells of 13 bytes, row-major
   (0x481550), W and H the level's (main+0x14233, +0x14237): LoadMap allocates it (0x48399D) and
   the teardown frees it (0x483E7E, inside 0x491B60), whose entry ends the recording first. The
   pool *(main+0x1420B) holds TAGPU_LIM_WRECKS records, allocated by 0x421F20 at the level load
   and freed by the teardown (0x4221F8). The feature defs *(main+0x1426F), 0x100 bytes each, are
   read below their count main+0x14253, which only a load grows (LoadFeature 0x4224B0's callers:
   the unit defs' 0x42CCEB, the map's 0x422026 and 0x4918D9, the schema's 0x4231DE and a saved
   game's 0x424DE1). Each capture checks that the grid, W, H, the pool and the def count are the
   ones the recording began with, and ends it otherwise (REC_END_FEATURES). A cell an argument
   names is bounded by W and H, and a record by the pool's count, before anything is read there;
   a cell's back-offsets to its anchor are data, bounded again.

   WHICH CELLS CHANGED. Comparing the whole grid every iteration costs more than the capture's
   budget (7 MB on Two Continents, 21 MB on Seven Islands), and a write watch would see the draw
   write the flags of every cell it draws, every frame (0x4698A4..0x4698B0). So each writer marks
   the cells it may write, from its arguments, before it runs: the spawn 0x423C50, the destroy
   0x4246B0, FeatureDie 0x423550, the burn 0x4233A0, the damage 0x4244B0 and the swap 0x423710
   the footprint of the feature at the cell they name (and the spawn its new def's), the stamp
   0x47CC30, the clear 0x47D0E0 and the re-claim 0x47C790 the unit's footprint (+0x76, +0x78,
   +0x7E, +0x80, each an i16) -- all a cell wider each way. A search of .text for every operand
   naming the grid, and of the callers of the cell getters and the feature functions, found no
   other writer in play but the draw's bit 2 (the engine map, *The feature grid*). The capture
   compares the marked cells with its copy. A rotating sweep compares 1/SWEEP_DIV of the grid each
   iteration as well: a changed cell no writer marked is recorded when it finds it, late, and
   counted (RT_UNOBSERVED), which names a writer the search missed.

   THE POOL is compared whole each iteration: 0x18000 bytes in stock and 0x60000 raised, 7 and 26
   us under Wine with both copies cold (MEASURED 2026-09-30, a scratch program on the reference
   setup).

   THE EVENTS. A feature function called outside every other one, and outside a unit's death or
   inside its explosion (below), is a root: recorded at its entry with its arguments and the site
   that called it, and at its return with its result. The feature tick 0x424050, whose one
   caller is 0x495585, is a root at every call. Between the two
   come the simulation RNG's draws at the feature code's six calls of it (feat_draw), what the
   root nested (a COB start from a damage the tick's blast applies, 0x489CE0), and, for the tick,
   the cells that changed since the iteration's images: the unit slots the unit tick moved, which
   reproduction reads (0x424189). That is what a replay needs to run the same call to the same
   end (tagpu_replay.c). The depth is a LIFO of the return addresses the observers' `after`
   restores, pushed only while this thread records: a call that is not recorded pushes nothing
   and has no `after`, and a recording neither starts nor ends inside a feature call -- it starts
   and ends at a capture, at the unit tick's call, or on the teardown's entry, outside every one.

   A UNIT'S DEATH is a frame too, from the destructor's entry, where its RK_EV_DEATH is taken, to
   its return, RK_EV_DEATH_END (death_enter). A replay replays the death through the destructor,
   which makes on the host what every machine makes: the feature calls the destructor makes
   itself -- its corpse, 0x486360 at 0x486D6F -- are its own and not recorded. Its explosion,
   0x49B000 at 0x486D50, is a frame of its own (rec_blast): the explosion's damage is computed
   only on the owner's machine (the owner gate, 0x49A01B..0x49A047, passes a type-3 owner's
   explosion by: a picture), so the feature calls its area damage makes are roots, recorded
   inside the death, and a replay makes them where the host's destructor makes its explosion --
   before the corpse, in the engine's order. No feature function calls the destructor, so a
   death is never inside a feature call, and the writer refuses a recording that says otherwise.
   Deaths do not nest in stock either: a lethal hit, the cargo loop's (0x48680B) too, marks its
   unit dying (+0x110 bit 14, 0x489EE2) for the unit tick's reaper (0x48AFD1). */

#define OFF_GRID       0x14287      /* the feature grid                                          */
#define OFF_NFDEFS     0x14253      /* the feature defs' count, i32                              */
#define OFF_FDEFS      0x1426F      /* FeatureDef[]                                              */
#define OFF_WRECKS     0x1420B      /* the wreck pool                                            */
#define OFF_WR_HEADS   0x14213      /* i32 the active, settled and free lists' heads, in a row   */
#define OFF_CURSOR     0x14257      /* the reproduction cursor, a cell index the tick walks down  */
#define FD_STRIDE      0x100
#define FD_NAMELEN     0x20
#define FD_FOOTX       0x94         /* i16 the footprint, in cells                               */
#define FD_FOOTZ       0x96
#define FD_MASK        0xFE         /* bit 0: a GAF feature                                      */
#define CELL_DEF       0x08
#define CELL_DZ        0x0A         /* a footprint cell's rows and columns back to its anchor     */
#define CELL_DX        0x0B         /*   (0x4246C6..0x4246E6)                                     */
#define DEF_FOOTPRINT  0xFFFE
#define DEF_NONE       0xFFFB       /* a def at or above it is no feature: none, void, border     */
#define U_FOOT_X       0x76         /* the footprint's origin and size, i16 each (0x47CC57..0x47CCA3) */
#define U_FOOT_Z       0x78
#define U_FOOT_W       0x7E
#define U_FOOT_H       0x80
#define FOOT_MAX       64           /* a footprint is data: a larger one marks this much          */
#define SWEEP_DIV      64           /* the sweep covers the grid in this many iterations          */
#define FDEPTH_MAX     REC_DEATHS_MAX   /* the frames the capture tracks: deaths, explosions and
                                       feature calls. The deepest chain is seven: a death, its
                                       explosion, the damage, FeatureDie, the swap, the spawn and
                                       the destroy */
#define WR_POOL_BYTES  ((size_t)TAGPU_LIM_WRECKS * REC_WRECK_BYTES)

typedef unsigned (__stdcall* PFN_Rand)(unsigned n);
#define E_Rand ((PFN_Rand)0x004B6C30u)

struct Feat {
    const unsigned char* grid;  /* the level's grid, its columns and rows                     */
    unsigned       w, h;
    const unsigned char* pool;  /* the wreck pool                                             */
    unsigned       ndefs;       /* the feature defs at the start                              */
    unsigned char* gshadow;     /* the grid's bytes as last compared                          */
    unsigned char* pshadow;     /* the pool's                                                 */
    unsigned short* lo;         /* per row, the columns marked since the last compare;        */
    unsigned short* hi;         /*   lo > hi when none                                        */
    unsigned       sweep;       /* the row the rotating sweep compares next                   */
    int32_t        lists[4];    /* the heads and the cursor as the ring last carried them     */
    unsigned       unobserved;  /* cells the sweep found changed that no writer had marked    */
    unsigned       roots, draws, cells, wrecks;   /* this recording's, for its end's line     */
    int            broken;      /* the calls nested past FDEPTH_MAX                           */
};

static unsigned short s_wgids[4];                                 /* a GAF record's id rows     */
static unsigned s_nwgids;

/* the cell and wreck masks from the exclusion file, as masks_init builds the others */
static void feat_masks_row(const ReplayLayoutRow* r)
{
    unsigned char* m = r->record == RL_CELL ? s_cmask :
                       r->record == RL_WRECK3DO ? s_w3mask : s_wgmask;
    unsigned k;
    if (r->record == RL_WRECKGAF && r->target == RL_T_GAFSEQ && s_nwgids < 4)
        s_wgids[s_nwgids++] = r->offset;
    for (k = 0; k < r->size; k++)
        m[r->offset + k] &= (unsigned char)(r->mask ? ~(r->mask >> (8 * k)) : 0);
}

static const unsigned char* feat_defs(const char* ta)
{
    return *(const unsigned char* const*)(ta + OFF_FDEFS);
}

static void cell_image(const unsigned char* raw, unsigned char* img)
{
    unsigned k;
    for (k = 0; k < REC_CELL_BYTES; k++) img[k] = raw[k] & s_cmask[k];
}

/* the def field that holds `v`, as its ID (tagpu_recfmt.h, REC_SEQ_FIRST) */
static unsigned seq_id(const unsigned char* def, unsigned v)
{
    unsigned i;
    if (!v) return REC_ID_NULL;
    for (i = 0; i < REC_SEQ_FIELDS; i++)
        if (*(const unsigned*)(def + REC_SEQ_FIRST + 4 * i) == v) return i + 1;
    s_bad++;
    return REC_ID_BAD;
}

/* A record's image: laid out by its def, a GAF feature's or a 3DO one's; a def outside the defs
   is imaged as a 3DO one (replay-layout.tsv). */
static void wreck_image(const Feat* f, const unsigned char* defs, const unsigned char* raw,
                        unsigned char* img)
{
    unsigned short def;
    const unsigned char* d;
    unsigned k;
    memcpy(&def, raw + REC_WRECK_DEF, 2);
    d = defs && def < f->ndefs ? defs + (size_t)def * FD_STRIDE : NULL;
    if (!d || !(d[FD_MASK] & 1)) {
        for (k = 0; k < REC_WRECK_BYTES; k++) img[k] = raw[k] & s_w3mask[k];
        return;
    }
    for (k = 0; k < REC_WRECK_BYTES; k++) img[k] = raw[k] & s_wgmask[k];
    for (k = 0; k < s_nwgids; k++) {
        unsigned id = s_wgids[k] == REC_WRECK_SEQ2 && !(raw[REC_WRECK_FLAGS] & REC_WRECK_SHADOW)
                    ? REC_ID_NULL : seq_id(d, *(const unsigned*)(raw + s_wgids[k]));
        memcpy(img + s_wgids[k], &id, 4);
    }
}

/* a record's ID from its address: one more than its index, 0 for NULL */
static unsigned wreck_id(const Feat* f, unsigned p)
{
    size_t off;
    if (!p) return REC_ID_NULL;
    if ((const unsigned char*)(size_t)p < f->pool) return REC_ID_BAD;
    off = (size_t)((const unsigned char*)(size_t)p - f->pool);
    if (off % REC_WRECK_BYTES || off >= WR_POOL_BYTES) return REC_ID_BAD;
    return (unsigned)(off / REC_WRECK_BYTES) + 1;
}

/* the rect [x0, x1) x [z0, z1), clamped to the grid, into the rows' marks */
static void mark(Feat* f, int x0, int z0, int x1, int z1)
{
    int z;
    if (x0 < 0) x0 = 0;
    if (z0 < 0) z0 = 0;
    if (x1 > (int)f->w) x1 = (int)f->w;
    if (z1 > (int)f->h) z1 = (int)f->h;
    for (z = z0; z < z1 && x0 < x1; z++) {
        if (x0 < f->lo[z]) f->lo[z] = (unsigned short)x0;
        if (x1 - 1 > f->hi[z]) f->hi[z] = (unsigned short)(x1 - 1);
    }
}

static int clamp_foot(int v) { return v < 1 ? 1 : v > FOOT_MAX ? FOOT_MAX : v; }

/* a def's footprint at (x, z), a cell wider each way */
static void mark_def(Feat* f, const unsigned char* defs, int x, int z, unsigned def)
{
    int fw = 1, fh = 1;
    if (defs && def < f->ndefs) {
        fw = clamp_foot(*(const short*)(defs + (size_t)def * FD_STRIDE + FD_FOOTX));
        fh = clamp_foot(*(const short*)(defs + (size_t)def * FD_STRIDE + FD_FOOTZ));
    }
    mark(f, x - 1, z - 1, x + fw + 1, z + fh + 1);
}

/* the feature at (x, z): its anchor, through a footprint cell's back-offsets, and its footprint */
static void mark_feature(Feat* f, const unsigned char* defs, int x, int z)
{
    const unsigned char* c;
    unsigned short def;
    if (x < 0 || z < 0 || x >= (int)f->w || z >= (int)f->h) return;
    c = f->grid + ((size_t)z * f->w + (unsigned)x) * REC_CELL_BYTES;
    memcpy(&def, c + CELL_DEF, 2);
    if (def == DEF_FOOTPRINT) {
        x -= c[CELL_DX];
        z -= c[CELL_DZ];
        if (x < 0 || z < 0) return;
        c = f->grid + ((size_t)z * f->w + (unsigned)x) * REC_CELL_BYTES;
        memcpy(&def, c + CELL_DEF, 2);
    }
    mark_def(f, defs, x, z, def);
}

/* the column and row of a cell the engine names by address; 0 when it is not one */
static int cell_xz(const Feat* f, unsigned p, int* x, int* z)
{
    size_t off;
    if ((const unsigned char*)(size_t)p < f->grid) return 0;
    off = (size_t)((const unsigned char*)(size_t)p - f->grid);
    if (off % REC_CELL_BYTES || off / REC_CELL_BYTES >= (size_t)f->w * f->h) return 0;
    *x = (int)(off / REC_CELL_BYTES % f->w);
    *z = (int)(off / REC_CELL_BYTES / f->w);
    return 1;
}

static void put_xz(unsigned char* b, int ok, int x, int z)
{
    unsigned short v[2];
    v[0] = ok ? (unsigned short)x : 0xFFFF;
    v[1] = ok ? (unsigned short)z : 0xFFFF;
    memcpy(b, v, 4);
}

/* a run of `n` cells of row z from column x, images from `raw`; 0 when it does not fit */
static int cells_put(Rec* r, unsigned x, unsigned z, const unsigned char* raw, unsigned n)
{
    unsigned char* at = ring_take(r, 8 + n * REC_CELL_BYTES);
    unsigned short h[4];
    unsigned k;
    if (!at) return 0;
    h[0] = (unsigned short)x;
    h[1] = (unsigned short)z;
    h[2] = (unsigned short)n;
    h[3] = 0;
    memcpy(at + 4, h, 8);
    for (k = 0; k < n; k++)
        cell_image(raw + (size_t)k * REC_CELL_BYTES, at + 12 + (size_t)k * REC_CELL_BYTES);
    ring_commit(r, at, RK_CELLS, 8 + n * REC_CELL_BYTES);
    r->feat->cells += n;
    return 1;
}

static int cell_moved(const unsigned char* a, const unsigned char* b)
{
    unsigned k;
    for (k = 0; k < REC_CELL_BYTES; k++)
        if ((a[k] ^ b[k]) & s_cmask[k]) return 1;
    return 0;
}

/* Row z's columns [x0, x1]: each run of cells whose image moved goes into the ring; with `update`
   the copy takes the row's bytes. `found` counts the cells recorded. 0 when the ring is full. */
static int row_compare(Rec* r, Feat* f, unsigned z, unsigned x0, unsigned x1, int update,
                       unsigned* found)
{
    size_t at = ((size_t)z * f->w + x0) * REC_CELL_BYTES;
    const unsigned char* live = f->grid + at;
    unsigned char* sh = f->gshadow + at;
    unsigned n = x1 - x0 + 1, i = 0;
    if (!memcmp(live, sh, (size_t)n * REC_CELL_BYTES)) return 1;
    while (i < n) {
        unsigned j = i;
        while (j < n && j - i < REC_CELLS_MAX &&
               cell_moved(live + (size_t)j * REC_CELL_BYTES, sh + (size_t)j * REC_CELL_BYTES))
            j++;
        if (j > i) {
            if (!cells_put(r, x0 + i, z, live + (size_t)i * REC_CELL_BYTES, j - i)) return 0;
            if (found) *found += j - i;
        } else {
            j = i + 1;                          /* equal, or the draw's bit alone */
        }
        if (update)
            memcpy(sh + (size_t)i * REC_CELL_BYTES, live + (size_t)i * REC_CELL_BYTES,
                   (size_t)(j - i) * REC_CELL_BYTES);
        i = j;
    }
    return 1;
}

/* every marked row's marked columns, at an iteration's images; the marks are cleared as they are
   compared */
static int cells_marked(Rec* r, Feat* f)
{
    unsigned z;
    for (z = 0; z < f->h; z++) {
        if (f->lo[z] > f->hi[z]) continue;
        if (!row_compare(r, f, z, f->lo[z], f->hi[z], 1, NULL)) return 0;
        f->lo[z] = 0xFFFF;
        f->hi[z] = 0;
    }
    return 1;
}

/* The same cells at the feature tick's entry, for its event: what changed since the iteration's
   images, which the tick reads. The copy and the marks stay as they were, so the next iteration's
   images carry every cell that changed since this one's, and the images stay the world at their
   iteration: a snapshot is one (tagpu_recwriter.c). */
static int cells_tick(Rec* r, Feat* f)
{
    unsigned z;
    for (z = 0; z < f->h; z++)
        if (f->lo[z] <= f->hi[z] && !row_compare(r, f, z, f->lo[z], f->hi[z], 0, NULL)) return 0;
    return 1;
}

static int cells_sweep(Rec* r, Feat* f)
{
    unsigned k, rows = (f->h + SWEEP_DIV - 1) / SWEEP_DIV, was = f->unobserved;
    for (k = 0; k < rows; k++) {
        if (!row_compare(r, f, f->sweep, 0, f->w - 1, 1, &f->unobserved)) return 0;
        f->sweep = (f->sweep + 1) % f->h;
    }
    if (!was && f->unobserved)
        tagpu_logf("recorder: the sweep found %u grid cells changed that no writer marked, first "
                   "at GameTime %d: a writer the capture does not observe", f->unobserved,
                   r->tick + 1);
    return 1;
}

/* The grid's first image: every cell, into the ring's `grid`, which RK_GRID hands the writer. */
static int grid_first(Rec* r, Feat* f)
{
    size_t n = (size_t)f->w * f->h, k;
    unsigned char* img = r->ring->grid;
    unsigned short wh[2];
    memcpy(f->gshadow, f->grid, n * REC_CELL_BYTES);
    for (k = 0; k < n; k++)
        cell_image(f->gshadow + k * REC_CELL_BYTES, img + k * REC_CELL_BYTES);
    for (k = 0; k < f->h; k++) {
        f->lo[k] = 0xFFFF;
        f->hi[k] = 0;
    }
    wh[0] = (unsigned short)f->w;
    wh[1] = (unsigned short)f->h;
    if (!ring_put(r, RK_GRID, wh, 4, NULL, 0)) return 0;
    f->cells += (unsigned)n;
    return 1;
}

/* every record whose bytes moved since the copy; the copy starts zero, so the first iteration
   carries every record that is not */
static int wrecks_cap(Rec* r, Feat* f, const char* ta)
{
    const unsigned char* defs = feat_defs(ta);
    size_t i;
    if (!memcmp(f->pool, f->pshadow, WR_POOL_BYTES)) return 1;
    for (i = 0; i < TAGPU_LIM_WRECKS; i++) {
        const unsigned char* raw = f->pool + i * REC_WRECK_BYTES;
        unsigned char* sh = f->pshadow + i * REC_WRECK_BYTES;
        unsigned char* at;
        unsigned short ix[2];
        if (!memcmp(raw, sh, REC_WRECK_BYTES)) continue;
        at = ring_take(r, 4 + REC_WRECK_BYTES);
        if (!at) return 0;
        ix[0] = (unsigned short)i;
        ix[1] = 0;
        memcpy(at + 4, ix, 4);
        wreck_image(f, defs, raw, at + 8);
        ring_commit(r, at, RK_WRECK, 4 + REC_WRECK_BYTES);
        memcpy(sh, raw, REC_WRECK_BYTES);
        f->wrecks++;
    }
    return 1;
}

static int lists_cap(Rec* r, Feat* f, const char* ta)
{
    int32_t v[4];
    memcpy(v, ta + OFF_WR_HEADS, 12);
    memcpy(&v[3], ta + OFF_CURSOR, 4);
    if (!memcmp(v, f->lists, sizeof v)) return 1;
    if (!ring_put(r, RK_FEATURES, v, sizeof v, NULL, 0)) return 0;
    memcpy(f->lists, v, sizeof v);
    return 1;
}

/* the level's grid, pool and defs are still the ones the recording began with */
static int feat_same(const Feat* f, const char* ta)
{
    return !f->broken && *(const unsigned char* const*)(ta + OFF_GRID) == f->grid &&
           *(const unsigned*)(ta + OFF_GRID_W) == f->w &&
           *(const unsigned*)(ta + OFF_GRID_H) == f->h &&
           *(const unsigned char* const*)(ta + OFF_WRECKS) == f->pool &&
           *(const unsigned*)(ta + OFF_NFDEFS) == f->ndefs;
}

/* 1 done, 0 the ring is full, -1 the features are not the recording's */
static int cap_features(Rec* r, const Level* L)
{
    Feat* f = r->feat;
    if (!feat_same(f, L->ta)) return -1;
    if (r->first) {
        if (!grid_first(r, f)) return 0;
    } else if (!cells_marked(r, f) || !cells_sweep(r, f)) {
        return 0;
    }
    return wrecks_cap(r, f, L->ta) && lists_cap(r, f, L->ta);
}

static unsigned feat_unobserved(const Rec* r) { return r->feat ? r->feat->unobserved : 0; }

static void feat_log(const Rec* r)
{
    const Feat* f = r->feat;
    if (!f) return;
    tagpu_logf("recorder: the features: a %u x %u grid; %u cell images, %u wreck records, %u feature "
               "roots, %u draws recorded; %u cells the sweep found unmarked", f->w, f->h, f->cells,
               f->wrecks, f->roots, f->draws, f->unobserved);
}

/* FNV-1a over every def's name, as RT_FEATURES states it */
static unsigned fdefs_hash(const char* ta, unsigned n)
{
    return names_hash((const char*)feat_defs(ta), n, FD_STRIDE, FD_NAMELEN);
}

static void feat_free(Feat* f)
{
    if (!f) return;
    if (f->gshadow) VirtualFree(f->gshadow, 0, MEM_RELEASE);
    if (f->pshadow) VirtualFree(f->pshadow, 0, MEM_RELEASE);
    if (f->lo) HeapFree(GetProcessHeap(), 0, f->lo);
    if (f->hi) HeapFree(GetProcessHeap(), 0, f->hi);
    HeapFree(GetProcessHeap(), 0, f);
}

/* Everything the features' capture needs, taken with the rest at the start: the copies, the
   marks and the ring's first-image buffer. NULL, logged, when the level has no grid or pool, or
   no memory. */
static Feat* feat_start(const char* ta, RecRing* g)
{
    Feat* f = (Feat*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *f);
    size_t cells;
    unsigned z;
    if (!f) return NULL;
    f->grid = *(const unsigned char* const*)(ta + OFF_GRID);
    f->w = *(const unsigned*)(ta + OFF_GRID_W);
    f->h = *(const unsigned*)(ta + OFF_GRID_H);
    f->pool = *(const unsigned char* const*)(ta + OFF_WRECKS);
    f->ndefs = *(const unsigned*)(ta + OFF_NFDEFS);
    if (!f->grid || !f->pool || !feat_defs(ta) || !f->w || !f->h || f->w > 0xFFFF ||
        f->h > 0xFFFF || f->ndefs > 0xFFFB) {
        tagpu_logf("recorder: FAILED the feature grid (%u x %u), the wreck pool or the feature "
                   "defs (%u) are not a level's: not recording", f->w, f->h, f->ndefs);
        feat_free(f);
        return NULL;
    }
    cells = (size_t)f->w * f->h;
    f->gshadow = (unsigned char*)VirtualAlloc(NULL, cells * REC_CELL_BYTES,
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    f->pshadow = (unsigned char*)VirtualAlloc(NULL, WR_POOL_BYTES, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE);
    f->lo = (unsigned short*)HeapAlloc(GetProcessHeap(), 0, f->h * sizeof *f->lo);
    f->hi = (unsigned short*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, f->h * sizeof *f->hi);
    g->grid = (unsigned char*)VirtualAlloc(NULL, cells * REC_CELL_BYTES, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_READWRITE);
    if (!f->gshadow || !f->pshadow || !f->lo || !f->hi || !g->grid) {
        tagpu_logf("recorder: FAILED no memory for the feature grid's copies (%u x %u cells): "
                   "not recording", f->w, f->h);
        feat_free(f);
        return NULL;
    }
    for (z = 0; z < f->h; z++) f->lo[z] = 0xFFFF;
    f->lists[0] = f->lists[1] = f->lists[2] = f->lists[3] = INT32_MIN;
    return f;
}

/* ---- the features' hooks ---- */

/* A feature call, a unit's death or its explosion in progress. A feature call made inside a
   feature call is part of it, and inside a death the destructor's own; inside the death's
   explosion it is a root (*The features*, A UNIT'S DEATH). `id` is a death's unit's ID. */
#define FR_DEATH       FR_ROOTS
#define FR_BLAST       (FR_ROOTS + 1)
typedef struct FFrame { unsigned ret, id; unsigned char root, recorded, owner; } FFrame;
static FFrame   s_fstack[FDEPTH_MAX];
static unsigned s_fdepth;           /* moved only on the recording's game thread (feat_enter) */
static unsigned char s_fphase;      /* the step's phase (tagpu_rec_phase), on the game thread    */

void __cdecl tagpu_rec_phase(unsigned phase) { s_fphase = (unsigned char)phase; }

/* The root's arguments after its site, into `b`, and the cells it may write marked; the bytes
   written. A pointer argument is read no further than the function reads it. */
static unsigned root_args(Rec* r, Feat* f, void* E, unsigned root, unsigned char* b)
{
    const unsigned char* defs = feat_defs(r->L.ta);
    int x = 0, z = 0, ok;
    unsigned v;
    switch (root) {
    case FR_SPAWN: {
        unsigned short def = (unsigned short)ARG(E, 8), turn[3] = { 0, 0, 0 }, pad = 0;
        const int* pos = (const int*)PTR(ARG(E, 12));
        const unsigned short* tn = (const unsigned short*)PTR(ARG(E, 16));
        int p3[3] = { 0, 0, 0 };
        ok = cell_xz(f, ARG(E, 4), &x, &z);
        put_xz(b, ok, x, z);
        memcpy(b + 4, &def, 2);
        b[6] = (unsigned char)(ARG(E, 20) & 0xF);
        b[7] = (unsigned char)((pos ? 1 : 0) | (tn ? 2 : 0));
        if (pos) memcpy(p3, pos, 12);                  /* 0x423E28..0x423E38 */
        if (tn) memcpy(turn, tn, 6);                   /* 0x423E9B..0x423EA2 */
        memcpy(b + 8, p3, 12);
        memcpy(b + 20, turn, 6);
        memcpy(b + 26, &pad, 2);
        if (ok) {
            mark_feature(f, defs, x, z);
            mark_def(f, defs, x, z, def);
        }
        return 28;
    }
    case FR_DESTROY:
        ok = cell_xz(f, ARG(E, 4), &x, &z);
        put_xz(b, ok, x, z);
        v = ARG(E, 8);
        memcpy(b + 4, &v, 4);
        if (ok) mark_feature(f, defs, x, z);
        return 8;
    case FR_DAMAGE:
        ok = cell_xz(f, ARG(E, 4), &x, &z);
        put_xz(b, ok, x, z);
        v = ARG(E, 8);
        memcpy(b + 4, &v, 4);
        v = ARG(E, 12);
        memcpy(b + 8, &v, 4);
        v = id_of(&r->L, RL_T_WEAPON, PTR(ARG(E, 16)));
        memcpy(b + 12, &v, 4);
        if (ok) mark_feature(f, defs, x, z);
        return 16;
    case FR_DIE:
    case FR_BURN:
    case FR_SWAP:
        v = ARG(E, 4);
        memcpy(b, &v, 4);
        x = (int)v;
        v = ARG(E, 8);
        memcpy(b + 4, &v, 4);
        z = (int)v;
        v = ARG(E, 12);
        memcpy(b + 8, &v, 4);
        mark_feature(f, defs, x, z);
        return 12;
    default:
        return 0;
    }
}

static int feat_enter(void* E, unsigned root)
{
    Rec* r = ev_rec();
    Feat* f;
    FFrame* fr;
    unsigned char b[8 + REC_ROOT_ARGS_MAX];
    unsigned n;
    if (!r || !(f = r->feat)) return 0;
    if (s_fdepth >= FDEPTH_MAX) {
        f->broken = 1;                  /* the next capture ends the recording */
        return 0;
    }
    n = root_args(r, f, E, root, b + 8);
    fr = &s_fstack[s_fdepth];
    fr->ret = ARG(E, 0);
    fr->id = 0;
    fr->root = (unsigned char)root;
    fr->recorded = !s_fdepth || s_fstack[s_fdepth - 1].root == FR_BLAST;
    if (fr->recorded) {
        unsigned site = ARG(E, 0) - 5;
        b[0] = (unsigned char)root;
        b[1] = s_fphase;
        b[2] = b[3] = 0;
        memcpy(b + 4, &site, 4);
        ev_put(r, RK_EV_FEATURE, b, 8 + n);
        f->roots++;
        if (root == FR_TICK && !r->ev_full && !cells_tick(r, f)) {
            r->ev_full = 1;
            tagpu_logf("recorder: the feature tick's cells did not fit in the ring after GameTime "
                       "%d: the recording ends at the next iteration", r->tick);
        }
    }
    s_fdepth++;
    return 1;
}

static void* __cdecl feat_leave(unsigned* regs)
{
    FFrame* fr = &s_fstack[--s_fdepth];
    Rec* r;
    if (fr->recorded && (r = ev_rec()) != NULL && r->feat) {
        unsigned char b[8];
        unsigned v = regs[7];
        /* 0x423C50 hands back the record it took or 0, and 0x423710 its spawn's, or 0 for no
           cell, or a value that is no record for a cell holding no feature (0x423734 ->
           0x4237C3), which wreck_id makes REC_ID_BAD on both machines; 0x4246B0 a 0 or 1
           (0x4246F2, 0x42471E, 0x42482A); the others end on a network send's or a field's
           value */
        if (fr->root == FR_SPAWN || fr->root == FR_SWAP) v = wreck_id(r->feat, v);
        else if (fr->root != FR_DESTROY) v = 0;
        b[0] = fr->root;
        b[1] = b[2] = b[3] = 0;
        memcpy(b + 4, &v, 4);
        ev_put(r, RK_EV_FEATURE_END, b, 8);
    }
    return (void*)(size_t)fr->ret;
}

/* Whether the stack has room for a death's frame, asked before its RK_EV_DEATH goes in, so that no
   death is published without the bracket that closes it. Without room the next capture ends the
   recording. */
static int death_room(Rec* r)
{
    if (s_fdepth < FDEPTH_MAX) return 1;
    r->feat->broken = 1;
    return 0;
}

/* The destructor's entry, once its RK_EV_DEATH is in: the death is a frame, so the feature calls
   it makes itself are its own (*The features*, A UNIT'S DEATH). Nothing about the grid is marked
   here: each call it makes marks its own. */
static int death_enter(Rec* r, void* E, unsigned id, unsigned owner)
{
    FFrame* fr = &s_fstack[s_fdepth++];
    (void)r;
    fr->ret = ARG(E, 0);
    fr->id = id;
    fr->root = FR_DEATH;
    fr->recorded = 1;
    fr->owner = (unsigned char)owner;
    return 1;
}

/* After the destructor: an owner whose live count it took to 0 was eliminated in this death
   (0x486DFC, 0x486E54). */
static void* __cdecl death_leave(unsigned* regs)
{
    FFrame* fr = &s_fstack[--s_fdepth];
    Rec* r = ev_rec();
    (void)regs;
    if (r) {
        ev_put(r, RK_EV_DEATH_END, &fr->id, 4);
        if (fr->owner < REC_PLAYERS &&
            !*(const unsigned short*)(r->L.ta + OFF_PLAYERS + (size_t)fr->owner * PL_STRIDE + PL_LIVE))
            r->defeat[fr->owner] = *(const unsigned*)(r->L.ta + OFF_GAMETIME);
    }
    return (void*)(size_t)fr->ret;
}

/* The destructor's explosion, 0x49B000(unit, selfd), `ret 8`, whose call at 0x486D50 comes here
   (THE INSTALL): a frame while this thread records. Then, on a puppet host replaying the death,
   the replay makes the feature calls the recording's explosion made (tagpu_replay_blast). */
typedef void (__stdcall* PFN_Blast)(char* unit, unsigned selfd);
#define E_Blast    ((PFN_Blast)0x0049B000u)
#define BLAST_CALL 0x00486D50u

static void __stdcall rec_blast(char* unit, unsigned selfd)
{
    Rec* r = ev_rec();
    int framed = 0;
    if (r && r->feat) {
        if (s_fdepth < FDEPTH_MAX) {
            FFrame* fr = &s_fstack[s_fdepth++];
            fr->ret = fr->id = 0;
            fr->root = FR_BLAST;
            fr->recorded = 0;
            framed = 1;
        } else {
            r->feat->broken = 1;
        }
    }
    E_Blast(unit, selfd);
    if (framed) s_fdepth--;
    tagpu_replay_blast(unit);
}

/* the feature functions' entries, by RecRoot, for the counts */
static const unsigned FEAT_VA[FR_ROOTS] = { 0x00424050u, 0x00423C50u, 0x004246B0u, 0x00423550u,
                                            0x004233A0u, 0x004244B0u, 0x00423710u };
static int feat_counted(void* E, unsigned root)
{
    cnt(FEAT_VA[root], ARG(E, 0), 0);
    return feat_enter(E, root);
}
static int __cdecl ob_tick(void* E) { return feat_counted(E, FR_TICK); }
static int __cdecl ob_spawn(void* E) { return feat_counted(E, FR_SPAWN); }
static int __cdecl ob_destroy(void* E) { return feat_counted(E, FR_DESTROY); }
static int __cdecl ob_die(void* E) { return feat_counted(E, FR_DIE); }
static int __cdecl ob_burn(void* E) { return feat_counted(E, FR_BURN); }
static int __cdecl ob_damage(void* E) { return feat_counted(E, FR_DAMAGE); }
static int __cdecl ob_swap(void* E) { return feat_counted(E, FR_SWAP); }

/* the stamp, the clear and the re-claim, each (unit): the unit's footprint, a cell wider. On a
   puppet level, where nothing records, the call's grid writes are undone at its return instead
   (tagpu_puppet.c, THE FOOTPRINT WRITERS), and foot_leave is that return. */
static int __cdecl ob_foot(void* E)
{
    Rec* r = ev_rec();
    const char* u = PTR(ARG(E, 4));
    if (r && r->feat && ev_unit(r, u)) {
        int x = *(const short*)(u + U_FOOT_X), z = *(const short*)(u + U_FOOT_Z);
        int fw = clamp_foot(*(const short*)(u + U_FOOT_W));
        int fh = clamp_foot(*(const short*)(u + U_FOOT_H));
        mark(r->feat, x - 1, z - 1, x + fw + 1, z + fh + 1);
    }
    return tagpu_puppet_foot_enter(u, ARG(E, 0));
}
static int __cdecl ob_stamp(void* E) { cnt(0x0047CC30u, ARG(E, 0), 0); return ob_foot(E); }
static int __cdecl ob_clear(void* E) { cnt(0x0047D0E0u, ARG(E, 0), 0); return ob_foot(E); }
static int __cdecl ob_reclaim(void* E) { cnt(0x0047C790u, ARG(E, 0), 0); return ob_foot(E); }

static void* __cdecl foot_leave(unsigned* regs)
{
    (void)regs;
    return (void*)(size_t)tagpu_puppet_foot_leave();
}

/* THE DRAWS. The feature code's six calls of the simulation RNG go through feat_draw
   (tagpu_rec_init). On a puppet host that is playing a root, the recording's value
   (tagpu_puppet_draw); otherwise the engine's own draw, recorded while this thread records. */
static unsigned __stdcall feat_draw(unsigned site, unsigned n)
{
    Rec* r;
    unsigned v;
    if (tagpu_puppet_draw(site, n, &v)) return v;
    v = E_Rand(n);
    if (s_fdepth && (r = ev_rec()) != NULL && r->feat) {
        unsigned char b[12];
        b[0] = (unsigned char)site;
        b[1] = b[2] = b[3] = 0;
        memcpy(b + 4, &n, 4);
        memcpy(b + 8, &v, 4);
        ev_put(r, RK_EV_DRAW, b, 12);
        r->feat->draws++;
    }
    return v;
}

static const unsigned DRAW_SITES[FS_SITES] = { 0x004234C2u, 0x00423A38u, 0x00423B40u,
                                               0x00424113u, 0x00424159u, 0x00424167u };

/* The features as a dump's records: every cell, every record whose image is not all zero, and
   the lists' heads and the cursor, read as the capture reads them and within the same counts. A
   dump is taken on a puppet host too, which records nothing, so this reads the level itself. */
static void dump_features(const Level* L, const char* name)
{
    static unsigned char img[REC_CELLS_MAX * REC_CELL_BYTES];
    static const unsigned char zero[REC_WRECK_BYTES];
    Feat f;
    unsigned x, z, i;
    int32_t v[4];
    memset(&f, 0, sizeof f);
    f.grid = *(const unsigned char* const*)(L->ta + OFF_GRID);
    f.w = *(const unsigned*)(L->ta + OFF_GRID_W);
    f.h = *(const unsigned*)(L->ta + OFF_GRID_H);
    f.pool = *(const unsigned char* const*)(L->ta + OFF_WRECKS);
    f.ndefs = *(const unsigned*)(L->ta + OFF_NFDEFS);
    if (!f.grid || !f.pool || !f.w || !f.h || f.w > 0xFFFF || f.h > 0xFFFF) {
        tagpu_logf("recorder: dump %s: the level has no feature grid or wreck pool: the dump leaves "
                   "them out", name);
        return;
    }
    for (z = 0; z < f.h; z++)
        for (x = 0; x < f.w; ) {
            unsigned n = f.w - x < REC_CELLS_MAX ? f.w - x : REC_CELLS_MAX, k;
            unsigned short pre[4];
            const unsigned char* raw = f.grid + ((size_t)z * f.w + x) * REC_CELL_BYTES;
            pre[0] = (unsigned short)x;
            pre[1] = (unsigned short)z;
            pre[2] = (unsigned short)n;
            pre[3] = 0;
            for (k = 0; k < n; k++)
                cell_image(raw + (size_t)k * REC_CELL_BYTES, img + (size_t)k * REC_CELL_BYTES);
            dump_rec(RK_CELLS, pre, 8, img, n * REC_CELL_BYTES);
            x += n;
        }
    for (i = 0; i < TAGPU_LIM_WRECKS; i++) {
        unsigned short pre[2] = { (unsigned short)i, 0 };
        wreck_image(&f, feat_defs(L->ta), f.pool + (size_t)i * REC_WRECK_BYTES, img);
        if (memcmp(img, zero, REC_WRECK_BYTES)) dump_rec(RK_WRECK, pre, 4, img, REC_WRECK_BYTES);
    }
    memcpy(v, L->ta + OFF_WR_HEADS, 12);
    memcpy(&v[3], L->ta + OFF_CURSOR, 4);
    dump_rec(RK_FEATURES, v, sizeof v, NULL, 0);
}

/* THE INSTALL, at DllMain's attach: every site byte-matched and every custom stub built before
   anything lands. A failure before that installs nothing; one while landing leaves what landed
   inert, since every hook records only during a recording and no recording starts without
   s_sites -- a released network test then reads a flag that stays 0. */
typedef struct ObserveSite {
    unsigned va;
    unsigned char n, stolen[10];
    tagpu_detour_before_fn fn;
    tagpu_detour_after_fn after;        /* the feature roots' depth (*The features*), and the
                                           footprint writers' undo on a puppet level             */
} ObserveSite;
static const ObserveSite OBSERVE_SITES[] = {
    { 0x004861B3u, 5, { 0x8B, 0xC6, 0x5F, 0x5E, 0x5D }, ev_create, NULL },
    { 0x004866D0u, 9, { 0x83, 0xEC, 0x68, 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 }, ev_death,
      death_leave },
    { 0x0041B8D0u, 6, { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C }, ev_built, NULL },
    { 0x0048B090u, 5, { 0x51, 0x8B, 0x44, 0x24, 0x0C }, ev_state, NULL },
    { 0x00456190u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, ev_start_idx, NULL },
    { 0x00456290u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, ev_start_args, NULL },
    { 0x00456218u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, ev_start_named_bc, NULL },
    { 0x0047F780u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, ev_reply0, NULL },
    { 0x0047F850u, 5, { 0x57, 0x8B, 0x7C, 0x24, 0x08 }, ev_reply1, NULL },
    { 0x0043E400u, 8, { 0x83, 0xEC, 0x10, 0x56, 0x8B, 0x74, 0x24, 0x18 }, ev_nano_query, NULL },
    { 0x004720D0u, 7, { 0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x1C }, ev_spray0, NULL },
    { 0x00472200u, 7, { 0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x20 }, ev_spray1, NULL },
    { 0x0043A216u, 8, { 0x8B, 0x4E, 0x0E, 0x51, 0xFF, 0x54, 0x10, 0x04 }, ev_nano_h0, NULL },
    { 0x0043B878u, 8, { 0x8B, 0x4E, 0x0E, 0x51, 0xFF, 0x54, 0x10, 0x04 }, ev_nano_h1, NULL },
    { 0x0043BB1Du, 8, { 0x8B, 0x56, 0x0E, 0x52, 0xFF, 0x54, 0x08, 0x04 }, ev_nano_h2, NULL },
    /* the features: the seven onto the puppet gates (tagpu_puppet.c), whose bytes they match */
    { 0x00424050u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, ob_tick, feat_leave },
    { 0x00423C50u, 5, { 0x83, 0xEC, 0x28, 0x53, 0x55 }, ob_spawn, feat_leave },
    { 0x004246B0u, 7, { 0x53, 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, ob_destroy, feat_leave },
    { 0x00423550u, 5, { 0x83, 0xEC, 0x08, 0x53, 0x55 }, ob_die, feat_leave },
    { 0x004233A0u, 8, { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04 }, ob_burn, feat_leave },
    { 0x004244B0u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, ob_damage, feat_leave },
    { 0x00423710u, 8, { 0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04 }, ob_swap, feat_leave },
    { 0x0047CC30u, 5, { 0x83, 0xEC, 0x18, 0x53, 0x55 }, ob_stamp, foot_leave },
    { 0x0047D0E0u, 10, { 0x83, 0xEC, 0x08, 0x53, 0x8B, 0x1D, 0xE8, 0x1D, 0x51, 0x00 }, ob_clear,
      foot_leave },
    { 0x0047C790u, 7, { 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10 }, ob_reclaim, foot_leave },
};
#define OBSERVE_SITE_COUNT (sizeof OBSERVE_SITES / sizeof OBSERVE_SITES[0])

/* THE SINKS, counted only: where an effect leaves the simulation for the screen or the speakers,
   or a script thread or a pool slot is taken -- what a replay must reach through the engine's own
   roots (CLAUDE.md, *Simulation changes stay replayable*). Each observer reads its return address
   and, for two, an aux; it changes nothing. */
static int __cdecl sink_count(void* E, unsigned va, unsigned aux)
{
    if (cnt_on()) cnt_add(va, cnt_caller(ARG(E, 0)), aux);
    return 0;
}

static void cnt_name(unsigned aux, const char* name, unsigned n)
{
    unsigned i;
    for (i = 0; i < s_cnt.nnames; i++)
        if (s_cnt.name[i].aux == aux) return;
    if (s_cnt.nnames >= CNT_NAMES) return;
    s_cnt.name[s_cnt.nnames].aux = aux;
    memcpy(s_cnt.name[s_cnt.nnames].name, name, n);
    s_cnt.name[s_cnt.nnames].name[n] = 0;
    s_cnt.nnames++;
}

/* 0x4B08C0(cob, idx), thiscall, the COB thread allocation: aux is FNV-1a of the script's name.
   The engine's own reads, no further, on the unit's live COB object inside the engine's call on
   it: idx bounded by the header's count as the function bounds it (0x4B08C5..0x4B08D0), the name
   at [[hdr+0x1C] + 4 idx] as Name2Index reads it (0x4B07C0..0x4B07D3), to its NUL, at most 31 bytes. */
static int __cdecl sink_cob(void* E)
{
    const char* cob = PTR(REG_ECX(E));
    const char* hdr;
    const char* name;
    int idx = (int)ARG(E, 4);
    unsigned h = 0x811C9DC5u, n = 0;
    if (!cnt_on()) return 0;
    if (cob && (hdr = *(const char* const*)(cob + 8)) != NULL && idx >= 0 &&
        idx < *(const int*)(hdr + 4) &&
        (name = (*(const char* const* const*)(hdr + 0x1C))[idx]) != NULL) {
        while (n < REC_COUNT_NAME - 1 && name[n]) h = (h ^ (unsigned char)name[n++]) * 0x01000193u;
        cnt_name(h, name, n);
    } else {
        h = 0;
    }
    cnt_add(0x004B08C0u, cnt_caller(ARG(E, 0)), h);
    return 0;
}

static int __cdecl sink_420A30(void* E) { return sink_count(E, 0x00420A30u, 0); }
/* 0x47F1A0 calls 0x47F0C0 at 0x47F1FA on every call, so that call is the same sound counted once */
static int __cdecl sink_47F0C0(void* E)
{
    return ARG(E, 0) == 0x0047F1FFu ? 0 : sink_count(E, 0x0047F0C0u, 0);
}
static int __cdecl sink_47F1A0(void* E) { return sink_count(E, 0x0047F1A0u, 0); }
static int __cdecl sink_47F210(void* E) { return sink_count(E, 0x0047F210u, 0); }
static int __cdecl sink_47F300(void* E) { return sink_count(E, 0x0047F300u, 0); }
static int __cdecl sink_49C740(void* E) { return sink_count(E, 0x0049C740u, 0); }
static int __cdecl sink_49CDE0(void* E) { return sink_count(E, 0x0049CDE0u, 0); }
static int __cdecl sink_49CC20(void* E) { return sink_count(E, 0x0049CC20u, 0); }
static int __cdecl sink_49C9C0(void* E) { return sink_count(E, 0x0049C9C0u, 0); }
static int __cdecl sink_499EB0(void* E) { return sink_count(E, 0x00499EB0u, 0); }
static int __cdecl sink_471FD0(void* E) { return sink_count(E, 0x00471FD0u, 0); }
static int __cdecl sink_472330(void* E) { return sink_count(E, 0x00472330u, 0); }
static int __cdecl sink_472430(void* E) { return sink_count(E, 0x00472430u, 0); }
static int __cdecl sink_472530(void* E) { return sink_count(E, 0x00472530u, 0); }
/* The flare, keyed by the caller of the explosion add 0x420A30 that emits it. The add emits it at
   0x420AE1 when its 4th argument is 0 and the point's y word +6 is above the sea level byte
   main+0x1427F (0x420ABE..0x420AD8), so each add's flares are compared beside the add itself and
   replay-absent.tsv's rows for the flying pieces' adds reach their flares and no other add's.
   THE BOUND on what the key means: a return to 0x420AE6 is the add's one call of the flare, and
   the add's frame there is fixed by its code (DISASSEMBLED): push edi 0x420A35, push esi 0x420A55,
   push ebx 0x420A58, the four arguments 0x420ADA..0x420AE0, and between them only 0x4B8B30, which
   returns `ret 0xC` on both exits (0x4B8B6B, 0x4B8B7E), and the sequence stub at 0x420AA2
   (tagpu_patches.c, which pushes and pops edx), so [esp+0x20] is the add's return address.
   cnt_sinks_init matches those bytes (s_flareAux); on a mismatch the flare is counted with aux 0.
   The word is a count key read from this thread's own stack, never dereferenced. */
static const unsigned char ADD_EDI[1] = { 0x57 };                       /* 0x420A35 push edi */
static const unsigned char ADD_A[77] = {                                /* 0x420A55..0x420AA1 */
    0x56, 0x2B, 0xC1, 0x53, 0x8B, 0x5C, 0x24, 0x10, 0x41, 0x8D, 0x14, 0x40, 0x89, 0x0F, 0x8B, 0xCB,
    0x8D, 0x74, 0x97, 0x04, 0x8B, 0x11, 0x8D, 0x46, 0x1C, 0x89, 0x56, 0x1C, 0x8B, 0x51, 0x04, 0x89,
    0x50, 0x04, 0x8B, 0x49, 0x08, 0x89, 0x48, 0x08, 0x8B, 0x44, 0x24, 0x14, 0x85, 0xC0, 0x74, 0x0E,
    0x6A, 0x00, 0x8D, 0x56, 0x04, 0x50, 0x52, 0xE8, 0x9F, 0x80, 0x09, 0x00, 0xEB, 0x07, 0xC7, 0x46,
    0x0C, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x18, 0x85, 0xC0, 0x7C, 0x15 };
static const unsigned char ADD_B[73] = {                                /* 0x420AA9..0x420AF1 */
    0x6A, 0x00, 0x8D, 0x4E, 0x10, 0x50, 0x51, 0xE8, 0x7B, 0x80, 0x09, 0x00, 0xEB, 0x07, 0xC7, 0x46,
    0x18, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x1C, 0x85, 0xC0, 0x75, 0x20, 0x8B, 0x15, 0xE8,
    0x1D, 0x51, 0x00, 0x66, 0x0F, 0xB6, 0x82, 0x7F, 0x42, 0x01, 0x00, 0x66, 0x39, 0x43, 0x06, 0x7E,
    0x0C, 0x6A, 0x09, 0x6A, 0x0F, 0x6A, 0x07, 0x53, 0xE8, 0x4A, 0x1B, 0x05, 0x00, 0xC7, 0x06, 0x00,
    0x00, 0x00, 0x00, 0x5B, 0x5E, 0x5F, 0xC2, 0x10, 0x00 };
static const unsigned char RET_0C[3] = { 0xC2, 0x0C, 0x00 };           /* 0x4B8B6B, 0x4B8B7E */
static int s_flareAux;                  /* the add's frame matched at attach */

static int __cdecl sink_472630(void* E)
{
    unsigned aux = s_flareAux && ARG(E, 0) == 0x00420AE6u ? cnt_caller(ARG(E, 0x20)) : 0;
    return sink_count(E, 0x00472630u, aux);
}
static int __cdecl sink_472810(void* E) { return sink_count(E, 0x00472810u, 0); }
static int __cdecl sink_4728F0(void* E) { return sink_count(E, 0x004728F0u, 0); }
static int __cdecl sink_4729D0(void* E) { return sink_count(E, 0x004729D0u, 0); }
static int __cdecl sink_472AB0(void* E) { return sink_count(E, 0x00472AB0u, 0); }
static int __cdecl sink_472C50(void* E) { return sink_count(E, 0x00472C50u, 0); }

/* the sinks' entries [DISASSEMBLED]: the particle spawn 0x420A30; the COB thread 0x4B08C0; the
   sound entries 0x47F0C0, 0x47F1A0, 0x47F210 (an entry: 0x47F1A0 ends with its `ret 8` at
   0x47F202, then padding to 0x47F20F) and 0x47F300; 0x47F210 and 0x47F300 call 0x47F0C0 too
   (returning to 0x47F279, 0x47F31F), so one sound is counted under both keys, on both machines; the projectile init 0x49C740 and the three shot paths
   0x49CDE0, 0x49CC20, 0x49C9C0; the impact 0x499EB0; and the ten particle emitters with a caller
   besides the two nano emitters (roots above), keyed by their callers (the flare 0x472630 from the
   add also by the add's caller, as aux): each makes exactly one pool
   allocation 0x470EB0 a call while the console's SFX byte 0x51E608 is clear (the only branch
   before it), so the emitters account for the pool's allocations call for call */
static const ObserveSite CNT_SINKS[] = {
    { 0x00420A30u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_420A30, NULL },
    { 0x004B08C0u, 5, { 0x56, 0x8B, 0x74, 0x24, 0x08 }, sink_cob, NULL },
    { 0x0047F0C0u, 7, { 0x8B, 0x4C, 0x24, 0x04, 0x83, 0xEC, 0x14 }, sink_47F0C0, NULL },
    { 0x0047F1A0u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, sink_47F1A0, NULL },
    { 0x0047F210u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, sink_47F210, NULL },
    { 0x0047F300u, 5, { 0xA1, 0x94, 0xE6, 0x51, 0x00 }, sink_47F300, NULL },
    { 0x0049C740u, 5, { 0x53, 0x8B, 0x5C, 0x24, 0x10 }, sink_49C740, NULL },
    { 0x0049CDE0u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, sink_49CDE0, NULL },
    { 0x0049CC20u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, sink_49CC20, NULL },
    { 0x0049C9C0u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, sink_49C9C0, NULL },
    { 0x00499EB0u, 7, { 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x10 }, sink_499EB0, NULL },
    { 0x00471FD0u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_471FD0, NULL },
    { 0x00472330u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_472330, NULL },
    { 0x00472430u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_472430, NULL },
    { 0x00472530u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_472530, NULL },
    { 0x00472630u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_472630, NULL },
    { 0x00472810u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_472810, NULL },
    { 0x004728F0u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_4728F0, NULL },
    { 0x004729D0u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_4729D0, NULL },
    { 0x00472AB0u, 7, { 0x83, 0xEC, 0x0C, 0x8B, 0x44, 0x24, 0x10 }, sink_472AB0, NULL },
    { 0x00472C50u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, sink_472C50, NULL },
};
#define CNT_SINK_COUNT (sizeof CNT_SINKS / sizeof CNT_SINKS[0])

/* each sink alone: one whose bytes are another's is left out, named, and its bit stays clear,
   which tarec reads as a site it cannot compare */
static void cnt_sinks_init(void)
{
    MEMORY_BASIC_INFORMATION mb;
    unsigned i, in = 0;
    if (VirtualQuery((const void*)cnt_sinks_init, &mb, sizeof mb)) {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)mb.AllocationBase;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const char*)dos + dos->e_lfanew);
        s_dllBase = (unsigned)(size_t)dos;
        s_dllSize = nt->OptionalHeader.SizeOfImage;
    }
    for (i = 0; i < CNT_SINK_COUNT; i++) {
        const ObserveSite* o = &CNT_SINKS[i];
        if (!tagpu_detour_bytes_ok(o->va, o->stolen, o->n) ||
            !tagpu_detour_observe(o->va, o->stolen, o->n, o->fn, NULL)) {
            tagpu_logf("recorder: the sink 0x%06X holds other bytes or refused its observer: its "
                       "counts are left out", o->va);
            continue;
        }
        s_cntSinks |= 1u << i;
        in++;
    }
    tagpu_logf("recorder: %u of %u sinks counted", in, (unsigned)CNT_SINK_COUNT);
    /* bit 0: the add's own entry observer, which keeps esp, is the only hook on the add */
    s_flareAux = (s_cntSinks & 1u) &&
                 tagpu_detour_bytes_ok(0x00420A35u, ADD_EDI, sizeof ADD_EDI) &&
                 tagpu_detour_bytes_ok(0x00420A55u, ADD_A, sizeof ADD_A) &&
                 tagpu_detour_bytes_ok(0x00420AA9u, ADD_B, sizeof ADD_B) &&
                 tagpu_detour_bytes_ok(0x004B8B6Bu, RET_0C, sizeof RET_0C) &&
                 tagpu_detour_bytes_ok(0x004B8B7Eu, RET_0C, sizeof RET_0C);
    tagpu_logf(s_flareAux ? "recorder: the flare 0x472630 is keyed by its explosion add's caller"
                          : "recorder: the explosion add 0x420A30 holds other bytes: its flare is "
                            "counted with aux 0");
}

/* the counts (THE COUNTS), the puppet's COB writes among them, and the names of the scripts */
static void dump_counts(void)
{
    static unsigned body[4 + REC_COUNTS_CHUNK * 4];
    unsigned i, k = 0, cob[TAGPU_PUPPET_COB_SITES], cobva[TAGPU_PUPPET_COB_SITES];
    unsigned ncob = tagpu_puppet_cob_writes(cobva, cob);
    body[0] = s_cnt.overflow;
    body[1] = s_cntSinks;
    body[2] = s_cnt.tid != 0;
    /* the rows, REC_COUNTS_CHUNK a record; the last record may hold none */
    for (i = 0; i < CNT_ROWS + ncob; i++) {
        unsigned* row = &body[4 + 4 * k];
        if (i < CNT_ROWS) {
            if (!s_cnt.row[i].n) continue;
            memcpy(row, &s_cnt.row[i], 16);
        } else {
            row[0] = cobva[i - CNT_ROWS];
            row[1] = row[2] = 0;
            row[3] = cob[i - CNT_ROWS];
        }
        if (++k == REC_COUNTS_CHUNK) {
            body[3] = k;
            dump_rec(RK_COUNTS, body, 16 + 16 * k, NULL, 0);
            k = 0;
        }
    }
    body[3] = k;
    dump_rec(RK_COUNTS, body, 16 + 16 * k, NULL, 0);
    for (i = 0; i < s_cnt.nnames; i++) {
        unsigned char b[4 + REC_COUNT_NAME];
        memcpy(b, &s_cnt.name[i].aux, 4);
        memcpy(b + 4, s_cnt.name[i].name, REC_COUNT_NAME);
        dump_rec(RK_COUNT_NAME, b, sizeof b, NULL, 0);
    }
}

/* the fire functions' network tests, `test byte [reg+0x2A44],1` then a je */
static const unsigned NET_TESTS[5] = { 0x0049D7A0u, 0x0049DA92u, 0x0049DC64u, 0x0049DE25u,
                                       0x0049DFAFu };

static int pristine(unsigned va, const unsigned char* bytes, unsigned n)
{
    return !tagpu_detour_landed(va, NULL, NULL) && memcmp(PTR(va), bytes, n) == 0;
}

int tagpu_rec_hooks_in(void) { return s_sites; }

void tagpu_rec_init(void)
{
    unsigned char* net[5] = { 0 };
    unsigned char* calls[START_SITE_COUNT] = { 0 };
    unsigned char* draws[FS_SITES] = { 0 };
    unsigned char* blast = NULL;
    unsigned i, k;
    static const unsigned char regs[8] = { 0x8F, 0x8E, 0x8D, 0, 0x8B, 0, 0, 0 };

    /* every site first */
    for (i = 0; i < OBSERVE_SITE_COUNT; i++)
        if (!tagpu_detour_bytes_ok(OBSERVE_SITES[i].va, OBSERVE_SITES[i].stolen,
                                   OBSERVE_SITES[i].n)) {
            tagpu_logf("recorder: the event site 0x%06X holds other bytes: no event hooks, and "
                       "nothing will be recorded", OBSERVE_SITES[i].va);
            return;
        }
    for (i = 0; i < 5; i++) {
        const unsigned char* b = (const unsigned char*)PTR(NET_TESTS[i]);
        if (tagpu_detour_landed(NET_TESTS[i], NULL, NULL) || b[0] != 0xF6 ||
            (b[1] & 0xF8) != 0x80 || b[2] != 0x44 || b[3] != 0x2A || b[4] || b[5] || b[6] != 1) {
            tagpu_logf("recorder: the network test 0x%06X holds other bytes: no event hooks, and "
                       "nothing will be recorded", NET_TESTS[i]);
            return;
        }
    }
    for (i = 0; i < START_SITE_COUNT; i++) {
        const StartSite* st = &START_SITES[i];
        unsigned char c[5], m[6] = { 0x8B, 0, 0x9A, 0x00, 0x00, 0x00 };
        int32_t rel = (int32_t)(st->target - (st->call + 5));
        c[0] = 0xE8;
        memcpy(c + 1, &rel, 4);
        m[1] = regs[st->reg];
        if (!pristine(st->call, c, 5) || !pristine(st->mov, m, 6)) {
            tagpu_logf("recorder: the %s start at 0x%06X holds other bytes: no event hooks, and "
                       "nothing will be recorded", st->what, st->call);
            return;
        }
    }
    for (i = 0; i < FS_SITES; i++) {
        unsigned char c[5];
        int32_t rel = (int32_t)((unsigned)(size_t)E_Rand - (DRAW_SITES[i] + 5));
        c[0] = 0xE8;
        memcpy(c + 1, &rel, 4);
        if (!pristine(DRAW_SITES[i], c, 5)) {
            tagpu_logf("recorder: the feature draw at 0x%06X holds other bytes: no event hooks, "
                       "and nothing will be recorded", DRAW_SITES[i]);
            return;
        }
    }
    {
        unsigned char c[5];
        int32_t rel = (int32_t)((unsigned)(size_t)E_Blast - (BLAST_CALL + 5));
        c[0] = 0xE8;
        memcpy(c + 1, &rel, 4);
        if (!pristine(BLAST_CALL, c, 5)) {
            tagpu_logf("recorder: the destructor's explosion call 0x%06X holds other bytes: no "
                       "event hooks, and nothing will be recorded", BLAST_CALL);
            return;
        }
    }

    /* the stubs of the custom sites */
    for (i = 0; i < 5; i++) {
        unsigned char* p = net[i] = tagpu_detour_stub();
        if (!p) goto fail;
        memcpy(p, PTR(NET_TESTS[i]), 7);                /* the network bit: ZF clear in a network game */
        p += 7;
        *p++ = 0x75; *p++ = 0x07;                       /* jnz over the next test               */
        p = tagpu_detour_cmp_flag(p, &s_rec_fired);     /* else ZF clear while recording        */
        *p++ = 0xE9;
        tagpu_detour_rel(p, NET_TESTS[i] + 7);          /* to the je, which reads ZF            */
    }
    for (i = 0; i < START_SITE_COUNT; i++) {
        unsigned char* p = calls[i] = tagpu_detour_stub();
        if (!p) goto fail;
        *p++ = 0x9C;                                    /* pushfd                               */
        *p++ = 0x60;                                    /* pushad                               */
        *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24;   /* lea eax,[esp+0x24]: the call's esp */
        *p++ = 0x68; memcpy(p, &i, 4); p += 4;          /* push the site                        */
        *p++ = 0x50;                                    /* push eax                             */
        *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)ev_start_site); p += 4;
        *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;          /* add esp,8                            */
        *p++ = 0x61;                                    /* popad                                */
        *p++ = 0x9D;                                    /* popfd                                */
        *p++ = 0xE9; tagpu_detour_rel(p, START_SITES[i].target);
    }
    for (i = 0; i < FS_SITES; i++) {
        unsigned char* p = draws[i] = tagpu_detour_stub();
        if (!p) goto fail;
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x04;   /* push dword [esp+4]: the n      */
        *p++ = 0x68; memcpy(p, &i, 4); p += 4;          /* push the site                        */
        *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)feat_draw); p += 4;
        *p++ = 0xC2; *p++ = 0x04; *p++ = 0x00;          /* ret 4, the RNG's own                 */
    }
    {
        unsigned char* p = blast = tagpu_detour_stub();
        if (!p) goto fail;
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x08;   /* push dword [esp+8]: selfd      */
        *p++ = 0xFF; *p++ = 0x74; *p++ = 0x24; *p++ = 0x08;   /* push dword [esp+8]: the unit   */
        *p++ = 0xE8; tagpu_detour_rel(p, (unsigned)(size_t)rec_blast); p += 4;
        *p++ = 0xC2; *p++ = 0x08; *p++ = 0x00;          /* ret 8, the explosion's own           */
    }

    /* then all of them */
    for (i = 0; i < OBSERVE_SITE_COUNT; i++)
        if (!tagpu_detour_observe(OBSERVE_SITES[i].va, OBSERVE_SITES[i].stolen,
                                  OBSERVE_SITES[i].n, OBSERVE_SITES[i].fn,
                                  OBSERVE_SITES[i].after)) {
            tagpu_logf("recorder: FAILED the event site 0x%06X after %u others were in: the hooks "
                       "that are in stay inert, and nothing will be recorded",
                       OBSERVE_SITES[i].va, i);
            goto fail;
        }
    for (i = 0; i < 5; i++) {
        if (!tagpu_detour_land(NET_TESTS[i], net[i], 7)) goto landfail;
        net[i] = NULL;
    }
    for (k = 0; k < START_SITE_COUNT; k++) {
        unsigned char c[5];
        int32_t rel = (int32_t)((unsigned)(size_t)calls[k] - (START_SITES[k].call + 5));
        c[0] = 0xE8;
        memcpy(c + 1, &rel, 4);
        if (!tagpu_detour_write(START_SITES[k].call, c, 5)) goto landfail;
        calls[k] = NULL;
    }
    for (k = 0; k < FS_SITES; k++) {
        unsigned char c[5];
        int32_t rel = (int32_t)((unsigned)(size_t)draws[k] - (DRAW_SITES[k] + 5));
        c[0] = 0xE8;
        memcpy(c + 1, &rel, 4);
        if (!tagpu_detour_write(DRAW_SITES[k], c, 5)) goto landfail;
        draws[k] = NULL;
    }
    {
        unsigned char c[5];
        int32_t rel = (int32_t)((unsigned)(size_t)blast - (BLAST_CALL + 5));
        c[0] = 0xE8;
        memcpy(c + 1, &rel, 4);
        if (!tagpu_detour_write(BLAST_CALL, c, 5)) goto landfail;
        blast = NULL;
    }
    s_sites = 1;
    tagpu_logf("recorder: %u event sites in",
               (unsigned)(OBSERVE_SITE_COUNT + 5 + START_SITE_COUNT + FS_SITES + 1));
    cnt_sinks_init();
    return;

landfail:
    tagpu_logf("recorder: FAILED a site could not be written: the hooks that are in stay inert, "
               "and nothing will be recorded");
fail:
    for (i = 0; i < 5; i++) tagpu_detour_stub_free(net[i]);
    for (i = 0; i < START_SITE_COUNT; i++) tagpu_detour_stub_free(calls[i]);
    for (i = 0; i < FS_SITES; i++) tagpu_detour_stub_free(draws[i]);
    tagpu_detour_stub_free(blast);
}

/* ---- the entry points ----------------------------------------------------------------------- */

/* A level is recorded only while this file is in the game's folder at its first iteration
   (`tacli arm <instance> recorder.on`): the exact stream is too large to write for every game
   (demo-recorder-skirmish-slice.md, *Measured*). It decides whether a game is recorded, not what
   the simulation runs, so it is not a simulation lever and not in the header's lever list. */
#define REC_ON "tagpu_recorder.on"

/* THE FORK'S HANDLE (a lifetime). A takeover's fork is written into the file it was played from,
   through a handle the takeover opened (tagpu_replay.c, TK0). Its owners, in order:
   1. the takeover, from TK0 to the arm (TK4b);
   2. s_fork, from tagpu_rec_fork_arm to the next capture point;
   3. the ring, from rec_start;
   4. the writer, from writer_main's start (tagpu_recwriter.c).
   Each move is a store on one thread and then a NULL where it came from; the one that crosses to
   the writer's thread is ordered by CreateThread, which the capture calls after the ring holds
   it. Each owner closes it on its own failure: the takeover when the arm refuses, the level's end
   for an arm no capture point took, ring_free before the writer starts, and the writer's fail and
   finish after. */

/* what a fork needs to be recorded at the next capture point, but the absence of a recording */
static int fork_ok(char* why, unsigned cap)
{
    if (!tagpu_reclaim_level_tracked())
        snprintf(why, cap, "the teardown wrap is not in (tagpu_reclaim.c), so nothing would end "
                 "it");
    else if (!tagpu_simreg_count())
        snprintf(why, cap, "the sim-state registry accepted nothing");
    else if (!s_sites)
        snprintf(why, cap, "the event hooks are not in (tagpu_rec_init)");
    else if (table_too_wide())
        snprintf(why, cap, "%s has rows wider than a record carries", table_too_wide()->name);
    else if (!fin_free())
        snprintf(why, cap, "the process is exiting, or %u earlier recordings' files are still "
                 "being written", REC_FINISHING);
    else
        return 1;
    return 0;
}

/* THE ORDERING that keeps a 1 true until the next capture point: only rec_start takes a finishing
   slot, on this thread, and a writer finishing on another thread only frees one; the other checks
   are fixed once DllMain's attach has run. The process's exit is the exception, and a fork then
   fails at rec_start, its handle closed there. */
int tagpu_rec_fork_ready(char* why, unsigned cap)
{
    if (s_state == ST_RECORDING) {
        snprintf(why, cap, "a recording is in progress");
        return 0;
    }
    return fork_ok(why, cap);
}

/* THE ORDERING of the arm (tagpu_replay.c, TK4b): it is the takeover's last act, after the puppet
   table is off and the replay has stepped down, and the capture that consumes it is the next
   capture point, GameTime T's. In a skirmish nothing runs between the GameTime store 0x4954C0 and
   the capture point 0x4954ED: 0x4954C6 skips the pump block when the step's argument is 0, and the
   skirmish loop passes 0 (0x49693C; DISASSEMBLED). So the timeline's first iteration is the world
   the takeover's rebuild made, complete as every recording's first is. */
int tagpu_rec_fork_arm(RecFork* f)
{
    if (s_state == ST_RECORDING || s_fork.file) return 0;
    s_fork = *f;
    f->file = NULL;
    s_state = ST_IDLE;
    tagpu_logf("recorder: timeline %u of %s armed for GameTime %u (parent %u)", s_fork.id,
               s_fork.path, s_fork.fork_at, s_fork.parent);
    return 1;
}

/* the recording's start at this capture point: the counts kept since the level's last start are
   this recording's from here */
static void rec_begin(const Level* L, int tick)
{
    s_bad = s_badlog = 0;
    InterlockedExchange(&s_offthread, 0);
    s_nano_unit = NULL;
    s_nano_out = NULL;
    s_rec = rec_start(L, tick);
    if (s_rec) {
        s_state = ST_RECORDING;
        s_rec_fired = 1;
        s_tid = s_rec->tid;
    }
}

static void rec_stop(unsigned reason)
{
    s_tid = 0;
    rec_end(s_rec, reason);
    s_rec = NULL;
    s_state = ST_DONE;
}

void __cdecl tagpu_rec_capture(void)
{
    char* ta = ta_main();
    Level L;
    int tick, ok, mode, t;
    unsigned w0;
    LARGE_INTEGER t0, t1;
    Rec* r;

    s_fphase = 0;
    if (!ta || !level_read(ta, &L)) return;
    masks_init();
    tick = *(int*)(ta + OFF_GAMETIME);
    mode = game_mode(ta);

    if (s_state == ST_IDLE && s_fork.file) {
        /* a fork's first iteration (tagpu_rec_fork_arm): checked once more by value, since the
           arm, and recorded whatever REC_ON says */
        char why[160];
        s_state = ST_DONE;
        cnt_start(&L, mode == MODE_SKIRMISH && s_sites);
        if (mode != MODE_SKIRMISH) snprintf(why, sizeof why, "the game is not a skirmish");
        if (mode != MODE_SKIRMISH || !fork_ok(why, sizeof why)) {
            tagpu_logf("recorder: FAILED timeline %u of %s is not recorded: %s", s_fork.id,
                       s_fork.path, why);
            CloseHandle(s_fork.file);
            s_fork.file = NULL;
        } else {
            rec_begin(&L, tick);
            if (s_fork.file) {          /* rec_start failed before the ring took it */
                CloseHandle(s_fork.file);
                s_fork.file = NULL;
            }
        }
    } else if (s_state == ST_IDLE) {
        /* A recording starts at a level's first iteration, or at a fork's (tagpu_rec_fork_arm),
           or not at all, and only when its end is certain: the teardown wrap ends it
           (tagpu_rec_level_end), before the unit array is freed, and without the wrap nothing
           would. A level's starts only when REC_ON asks. */
        s_state = ST_DONE;
        cnt_start(&L, mode == MODE_SKIRMISH && s_sites &&
                      (tagpu_puppet_on() || tagpu_opt_on(REC_ON)));
        if (mode == MODE_SKIRMISH) {
            if (tagpu_puppet_on())
                tagpu_logf("recorder: not recording: this level plays a recording");
            else if (!tagpu_opt_on(REC_ON))
                tagpu_logf("recorder: not recording: %s is absent", REC_ON);
            else if (!tagpu_reclaim_level_tracked())
                tagpu_logf("recorder: not recording: the teardown wrap is not in (tagpu_reclaim.c), "
                           "so nothing would end a recording");
            else if (!tagpu_simreg_count())
                tagpu_logf("recorder: not recording: the sim-state registry accepted nothing");
            else if (!s_sites)
                tagpu_logf("recorder: not recording: the event hooks are not in (tagpu_rec_init)");
            else
                rec_begin(&L, tick);
        }
    }
    dumps_take(&L, tick);
    if (s_state != ST_RECORDING) return;

    r = s_rec;
    if (r->ev_full) { rec_stop(REC_END_RING); return; }
    if (InterlockedCompareExchange(&r->ring->failed, 0, 0)) { rec_stop(REC_END_WRITE); return; }
    if (mode != MODE_SKIRMISH) { rec_stop(REC_END_MODE); return; }
    if (L.begin != r->begin || L.nslots != r->nslots || tick <= r->tick) {
        rec_stop(REC_END_LEVEL);
        return;
    }
    /* the last step's events end its iteration: out they go, before this one starts */
    ring_publish(r, 0);
    r->L = L;
    QueryPerformanceCounter(&t0);
    w0 = r->w;
    ok = ring_put(r, RK_TICK, &tick, 4, NULL, 0) && cap_units(r, &L) && cap_players(r, &L);
    t = ok ? cap_orders(r, &L) : 0;
    if (t < 0) { r->w = w0; rec_stop(REC_END_ORDERS); return; }
    t = t ? cap_tables(r, &L) : 0;
    if (t < 0) { r->w = w0; rec_stop(REC_END_TABLE); return; }
    t = t ? cap_verify(r, &L) : 0;
    if (t < 0) { r->w = w0; rec_stop(REC_END_ORDERS); return; }
    t = t ? cap_features(r, &L) : 0;
    if (t < 0) { r->w = w0; rec_stop(REC_END_FEATURES); return; }
    t = t ? cap_globals(r, L.ta) : 0;
    t = t ? cap_proj(r, &L) : 0;
    if (t < 0) { r->w = w0; rec_stop(REC_END_PROJ); return; }
    t = t ? cap_mapped(r, &L) : 0;
    if (t < 0) { r->w = w0; rec_stop(REC_END_MAPPED); return; }
    if (!t) {
        /* the iteration does not fit: the recording ends after the last whole one, and what this
           one wrote is dropped unpublished */
        r->w = w0;
        rec_stop(REC_END_RING);
        return;
    }
    ring_publish(r, 0);
    QueryPerformanceCounter(&t1);
    r->iterations++;
    if (r->first) {
        r->cost_first = t1.QuadPart - t0.QuadPart;
    } else {
        r->cost += t1.QuadPart - t0.QuadPart;
        if (t1.QuadPart - t0.QuadPart > r->cost_max) {
            r->cost_max = t1.QuadPart - t0.QuadPart;
            r->max_tick = tick;
            r->max_bytes = r->w - w0;
        }
        r->pages += r->pages_now;
        if (r->pages_now > r->pages_max) r->pages_max = r->pages_now;
    }
    r->tick = tick;
    r->first = 0;
}

/* The level's last step has run, so its events end its iteration, unless one of them did not fit. */
#ifdef TAGPU_COVER
/* The coverage build only: the writer's end of the file (finish) is part of what a level's
   coverage reports, so the level end waits for the file it has just ended, by the writer's own
   `done` event. Only the game thread takes or closes a slot (fin_take), so the handle stays open
   through the wait. */
static void cover_wait(int fin)
{
    HANDLE h;
    fin_lock();
    h = fin >= 0 && fin < REC_FINISHING ? s_fin[fin] : NULL;
    fin_unlock();
    if (h) WaitForSingleObject(h, INFINITE);
}
#endif

void tagpu_rec_level_end(void)
{
    unsigned i, k = 0;
    /* a timed dump does not outlive its level, whose GameTime the next level starts again; an
       untimed one is taken at the next level's first capture point */
    for (i = 0; i < s_narmed; i++) {
        if (s_armed[i].at)
            tagpu_logf("recorder: dump %s for GameTime %u not taken: the level ended",
                       s_armed[i].name, s_armed[i].at);
        else
            s_armed[k++] = s_armed[i];
    }
    s_narmed = k;
    if (s_fork.file) {
        tagpu_logf("recorder: timeline %u of %s is not recorded: the level ended before its first "
                   "iteration", s_fork.id, s_fork.path);
        CloseHandle(s_fork.file);
        s_fork.file = NULL;
    }
    s_tid = 0;
    s_cnt.tid = 0;
    if (s_state == ST_RECORDING && s_rec) {
        int fin = s_rec->fin;
        if (!s_rec->ev_full) ring_publish(s_rec, 0);
        rec_end(s_rec, s_rec->ev_full ? REC_END_RING : REC_END_TEARDOWN);
#ifdef TAGPU_COVER
        cover_wait(fin);
#else
        (void)fin;
#endif
    }
    s_rec = NULL;
    s_state = ST_IDLE;
    tagpu_cover_flush();    /* after the recording's end, so its end is in the level's coverage */
}
