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
   - For the header, once: the unit-type definitions' names (UNITINFOCount of them), the weapons'
     names (the array's slots, tagpu_limits.h), the skirmish settings and the game's options.
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
#include "tagpu_recfmt.h"
#include "tagpu_replay_layout.h"
#include "tagpu_simreg.h"
#include "tagpu_limits.h"
#include "tagpu_log.h"
#include "tagpu_reclaim.h"
#include "tagpu_opt.h"
#include "tagpu_detour.h"
#include "tagpu_patches.h"
#include "tagpu_puppet.h"
#include "crc32.h"
#include "git.h"

#define TA_MAINPP      0x00511DE8u
#define OFF_PLAYERS    0x1B63       /* the eleven player records, 0x14B apart                     */
#define PL_STRIDE      0x14B
#define PL_ALL         11
#define PL_INFO        0x27         /* PlayerInfo*: +0x95 the side, +0x96 the logo (the saver's)  */
#define PL_NAME        0x2B         /* the long name the join stores (0x450BA3), 0x1E bytes         */
#define PL_NAMELEN     0x1E
#define PL_CONTROLLER  0x73
#define PL_ALLY        0x108        /* the alliance row, 11 bytes (the saver's `Alliances`)        */
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
#define MODE_SKIRMISH  2
#define U_MOVER        0x00         /* the mover, a record of its own (the image's tail)           */
#define U_ORDERS       0x5C         /* the order lists' heads: the main list and the sub list      */
#define U_SUBORDERS    0x60
#define U_OWNER        0x96         /* the owning player's record                                 */
#define U_COB          0x9A
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
#define DUMP_POLL      32           /* captures between two looks for the trigger                 */
#define HEADER_MAX     0xFF00       /* an RK_HEADER's body; its length is a u16                    */
#define IMG_DWORDS     (REC_PLAYER_DWORDS > REC_UNIT_DWORDS ? REC_PLAYER_DWORDS : REC_UNIT_DWORDS)
#define ORD_SCRATCH    (REC_ORDERS_MAX * REC_ORDER_IMAGE)

/* ---- the layout, as masks ------------------------------------------------------------------- */

/* Per image dword, the bits that are values or id bytes; per id row, where it is in the image and
   its table. Three images: a unit's (its record, then its mover's), a player's (its record, then
   its PlayerInfo's) and an order node's. Built once from the generated table, which the build keeps
   equal to replay-layout.tsv. */
#define RL_ROWS (sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0])
typedef struct { unsigned short off; unsigned char table; } IdRow;
enum { IMG_UNIT, IMG_PLAYER, IMG_ORDER, IMG_KINDS };
static unsigned s_umask[REC_UNIT_DWORDS], s_pmask[REC_PLAYER_DWORDS];
static unsigned s_omask[REC_ORDER_IMAGE / 4];
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
    for (i = 0; i < RL_ROWS; i++) {
        const ReplayLayoutRow* r = &tagpu_replay_layout[i];
        unsigned img = r->record == RL_PLAYER || r->record == RL_INFO ? IMG_PLAYER :
                       r->record == RL_ORDER ? IMG_ORDER : IMG_UNIT;
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
    static const char* const what[IMG_KINDS] = { "a unit's", "a player's", "an order node's" };
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

/* The tables a recording carries: SIM_STATE, less the engine pools the raised limits moved into
   the DLL (their record layouts are not yet written anywhere) and TLS slots (per thread). */
static int table_recorded(const SimDesc* d)
{
    return d->cls == SIM_STATE && !(d->flags & (SIMD_ENGINE | SIMD_TLS));
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
} Table;

static int table_grow(Table* tb, unsigned bytes);

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
} Rec;

static void olist_drop(Rec* r, unsigned slot);

enum { ST_IDLE, ST_RECORDING, ST_DONE };        /* DONE: the rest of this level is not recorded */
static Rec*     s_rec;
static int      s_state = ST_IDLE;
static unsigned s_polls;

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

static void header_build(const Level* L, int tick, const char* map, const FILETIME* now, Tlv* t)
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
}

/* One RT_TABLE: the registry index the stream's table records carry, and what the declaration
   is. The index is this build's registry order, which another build numbers otherwise, so a
   reader pairs two builds' tables by name. */
static void table_entry(Tlv* t, unsigned index, const SimDesc* d)
{
    unsigned char b[12 + 128];
    unsigned short ix = (unsigned short)index, fl = d->flags;
    unsigned n = (unsigned)bounded_len(d->name, 128);
    memcpy(b, &ix, 2);
    b[2] = d->cls;
    b[3] = d->key;
    memcpy(b + 4, &d->stride, 4);
    memcpy(b + 8, &fl, 2);
    b[10] = b[11] = 0;
    memcpy(b + 12, d->name, n);
    tlv(t, RT_TABLE, b, 12 + n);
}

static void header_tables(const Rec* r, Tlv* t)
{
    unsigned i;
    for (i = 0; i < r->ntab; i++) table_entry(t, r->tab[i].index, r->tab[i].d);
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
   while REC_FINISHING files are still being written. */
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
    if (r->wake) CloseHandle(r->wake);
    if (r->done) CloseHandle(r->done);
    HeapFree(GetProcessHeap(), 0, r);
}

static void ring_free(RecRing* g)
{
    if (!g) return;
    if (g->buf) VirtualFree(g->buf, 0, MEM_RELEASE);
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
    snprintf(g->path, sizeof g->path, "replays\\%04u-%02u-%02u %02u%02u%02u %s.imprec", lt.wYear,
             lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond, map);
    t.b = hb;
    t.n = 0;
    t.cap = sizeof hb;
    t.full = 0;
    header_build(L, tick, map, &now, &t);
    header_tables(r, &t);
    if (t.full) {
        tagpu_logf("recorder: FAILED the header does not fit in %u bytes: not recording", t.cap);
        goto fail;
    }
    if (!ring_put(r, RK_HEADER, NULL, 0, hb, t.n)) goto fail;
    ring_publish(r, 1);
    if (!tagpu_recwriter_start(g)) {
        tagpu_logf("recorder: FAILED no writer thread (%lu): not recording", GetLastError());
        goto fail;
    }
    tagpu_logf("recorder: recording %s from GameTime %d: %u slots, %u tables", g->path, tick,
               L->nslots, r->ntab);
    return r;

fail:
    ring_free(g);
    rec_free(r);
    return NULL;
}

/* The capture's last act on a recording: RK_END, published; the ring is the writer's from here
   (the hand-over, tagpu_recorder.h), and the writer's `done` goes to the exit's list. What was
   never published is dropped: an unfinished iteration's records, and a step's events when the
   recording ends before the next capture's checks pass (a step's events are published by that
   capture, before its images, and by the teardown's end). The ring's reserve holds the end
   whatever the published part left. The event hooks are off before this runs (s_tid). */
static void rec_end(Rec* r, unsigned reason)
{
    unsigned char b[20], * at;
    unsigned short why = (unsigned short)reason, z = 0;
    unsigned pos, off = (unsigned)InterlockedCompareExchange(&s_offthread, 0, 0);
    s_rec_fired = 0;
    r->w = (unsigned)InterlockedCompareExchange(&r->ring->wpub, 0, 0);
    memcpy(b, &why, 2);
    memcpy(b + 2, &z, 2);
    memcpy(b + 4, &s_bad, 4);
    memcpy(b + 8, &r->high, 4);
    memcpy(b + 12, &off, 4);
    memcpy(b + 16, &r->uncredited, 4);
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
                                            "INCOMPLETE, the game stopped being a skirmish", s_bad,
               off, r->uncredited, r->oodd);
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
        if (tb->unwatched ||
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

/* ---- the harness's dump --------------------------------------------------------------------- */

#define DUMP_TRIGGER SIM_TOOL_FILE("tagpu_replay.trigger")

static FILE*         s_dump;
static unsigned      s_dump_raw;
static unsigned long s_dump_crc;

static void dump_bytes(const void* p, unsigned n)
{
    if (!n) return;
    fwrite(p, 1, n, s_dump);
    s_dump_crc = Crc32_ComputeBuf(s_dump_crc, p, n);
    s_dump_raw += n;
}

static void dump_rec(unsigned kind, const void* pre, unsigned npre, const void* pay, unsigned npay)
{
    static const unsigned char zero[4];
    RecHdr h;
    unsigned body = npre + npay;
    h.kind = (uint16_t)kind;
    h.len = (uint16_t)body;
    dump_bytes(&h, 4);
    dump_bytes(pre, npre);
    dump_bytes(pay, npay);
    dump_bytes(zero, ((4 + body + 3) & ~3u) - 4 - body);
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

/* The live world's images as a stored RB_SNAPSHOT: what the recording must read back to at this
   GameTime. Before it, a stored RB_HEADER with the format, the build and the tables, so that a
   dump taken by another build than the recording's (a playing, D8) is read with its own table
   names. Written on the game thread, which is tooling's price: a dump is the harness's. */
static void dump_write(const Level* L, int tick, const char* name)
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
        return;
    }
    t.b = hb;
    t.n = 0;
    t.cap = sizeof hb;
    t.full = 0;
    tlv_u32(&t, RT_FORMAT, REC_FORMAT);
    tlv(&t, RT_BUILD, GIT_COMMIT, (unsigned)strlen(GIT_COMMIT));
    for (i = 0; i < n; i++) {
        const SimDesc* d = tagpu_simreg_at(i);
        if (d && table_recorded(d)) table_entry(&t, i, d);
    }
    if (t.full) {
        tagpu_logf("recorder: dump %s refused: its header does not fit in %u bytes", name, t.cap);
        return;
    }
    lists = (unsigned char*)VirtualAlloc(NULL, ORD_SCRATCH, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
    if (!lists) { tagpu_logf("recorder: dump %s: no memory for the order lists", name); return; }
    CreateDirectoryA("replays", NULL);
    snprintf(path, sizeof path, "replays\\dump-%s.bin", name);
    s_dump = fopen(path, "wb");
    if (!s_dump) {
        tagpu_logf("recorder: dump %s: cannot write %s", name, path);
        VirtualFree(lists, 0, MEM_RELEASE);
        return;
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
    tagpu_logf("recorder: dump %s at GameTime %d -> %s (%u bytes of records)", name, tick, path,
               s_dump_raw);
}

/* `dump <name>`, a name of letters, digits, '-' and '_'; the trigger is consumed either way */
static void dump_poll(const Level* L, int tick)
{
    char line[96], name[40];
    FILE* f;
    unsigned i;
    if (GetFileAttributesA(DUMP_TRIGGER) == INVALID_FILE_ATTRIBUTES) return;
    f = fopen(DUMP_TRIGGER, "rb");
    line[0] = 0;
    if (f) {
        size_t n = fread(line, 1, sizeof line - 1, f);
        line[n] = 0;
        fclose(f);
    }
    DeleteFileA(DUMP_TRIGGER);
    if (sscanf(line, "dump %39s", name) != 1) {
        tagpu_logf("recorder: %s: not understood (only `dump <name>`)", DUMP_TRIGGER);
        return;
    }
    for (i = 0; name[i]; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= 'A' && name[i] <= 'Z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '-' || name[i] == '_')) {
            tagpu_logf("recorder: dump: the name %s is not letters, digits, - and _", name);
            return;
        }
    dump_write(L, tick, name);
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

/* The create 0x485F50's success exit 0x4861B3 (`mov eax,esi`): esi is the unit it made, in the
   slot it actually took -- B4's hit_take may move a first-free create past its first choice. The
   function has pushed five registers, so its arguments start at +0x18; the first, the owner, is
   overwritten in the body (0x485F9C), so the owner is the unit's own +0x96. */
static int __cdecl ev_create(void* E)
{
    Rec* r = ev_rec();
    const char* unit;
    unsigned id, owner, v;
    unsigned char b[24];
    if (!r) return 0;
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
static int __cdecl ev_death(void* E)
{
    Rec* r = ev_rec();
    const unsigned char* msg;
    unsigned short slot, ks;
    unsigned id, kid;
    unsigned char b[12 + REC_UNIT_IMAGE];
    if (!r) return 0;
    msg = (const unsigned char*)PTR(ARG(E, 4));
    memcpy(&slot, msg + 1, 2);
    if (!slot || slot >= r->L.nslots || !unit_live(&r->L, slot)) return 0;
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
    return 0;
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
    ev_start(PTR(ARG(E, 4)), ARG(E, 8), 0, NULL);
    return 0;
}

/* 0x456290(unit, idx, argc, a0..a3) */
static int __cdecl ev_start_args(void* E)
{
    ev_start(PTR(ARG(E, 4)), ARG(E, 8), ARG(E, 0xC), (const unsigned char*)E + 0x10);
    return 0;
}

/* 0x456218, inside 0x456200(unit, name, argc, a0..a3) once 0x4B07C0 has turned the name into
   the index, which it broadcasts: esi the unit, eax the index, and the arguments 0x1C further up
   (`sub esp,0x18; push esi`) */
static int __cdecl ev_start_named_bc(void* E)
{
    ev_start(PTR(REG_ESI(E)), REG_EAX(E), ARG(E, 0x28), (const unsigned char*)E + 0x2C);
    return 0;
}

/* The build-done 0x41B8D0(builder, built): its own tests make a call on a dead or absent unit
   inert, so every call is recorded as made. */
static int __cdecl ev_built(void* E)
{
    Rec* r = ev_rec();
    unsigned b[2];
    if (!r) return 0;
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
    Rec* r = ev_rec();
    const char* unit;
    unsigned id;
    unsigned char was, now, b[8];
    if (!r) return 0;
    unit = PTR(REG_ECX(E));
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

static int __cdecl ev_reply0(void* E) { ev_reply(E, 0); return 0; }
static int __cdecl ev_reply1(void* E) { ev_reply(E, 1); return 0; }

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
    Rec* r = ev_rec();
    const char* unit;
    unsigned id;
    if (!r) return 0;
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

static int __cdecl ev_spray0(void* E) { ev_spray(E, 0); return 0; }
static int __cdecl ev_spray1(void* E) { ev_spray(E, 1); return 0; }

/* THE INSTALL, at DllMain's attach: every site byte-matched and every custom stub built before
   anything lands. A failure before that installs nothing; one while landing leaves what landed
   inert, since every hook records only during a recording and no recording starts without
   s_sites -- a released network test then reads a flag that stays 0. */
typedef struct ObserveSite {
    unsigned va;
    unsigned char n, stolen[9];
    tagpu_detour_before_fn fn;
} ObserveSite;
static const ObserveSite OBSERVE_SITES[] = {
    { 0x004861B3u, 5, { 0x8B, 0xC6, 0x5F, 0x5E, 0x5D }, ev_create },
    { 0x004866D0u, 9, { 0x83, 0xEC, 0x68, 0x8B, 0x15, 0xE8, 0x1D, 0x51, 0x00 }, ev_death },
    { 0x0041B8D0u, 6, { 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C }, ev_built },
    { 0x0048B090u, 5, { 0x51, 0x8B, 0x44, 0x24, 0x0C }, ev_state },
    { 0x00456190u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, ev_start_idx },
    { 0x00456290u, 5, { 0xA1, 0xE8, 0x1D, 0x51, 0x00 }, ev_start_args },
    { 0x00456218u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, ev_start_named_bc },
    { 0x0047F780u, 6, { 0x8B, 0x0D, 0xE8, 0x1D, 0x51, 0x00 }, ev_reply0 },
    { 0x0047F850u, 5, { 0x57, 0x8B, 0x7C, 0x24, 0x08 }, ev_reply1 },
    { 0x0043E400u, 8, { 0x83, 0xEC, 0x10, 0x56, 0x8B, 0x74, 0x24, 0x18 }, ev_nano_query },
    { 0x004720D0u, 7, { 0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x1C }, ev_spray0 },
    { 0x00472200u, 7, { 0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x20 }, ev_spray1 },
    { 0x0043A216u, 8, { 0x8B, 0x4E, 0x0E, 0x51, 0xFF, 0x54, 0x10, 0x04 }, ev_nano_handler },
    { 0x0043B878u, 8, { 0x8B, 0x4E, 0x0E, 0x51, 0xFF, 0x54, 0x10, 0x04 }, ev_nano_handler },
    { 0x0043BB1Du, 8, { 0x8B, 0x56, 0x0E, 0x52, 0xFF, 0x54, 0x08, 0x04 }, ev_nano_handler },
};
#define OBSERVE_SITE_COUNT (sizeof OBSERVE_SITES / sizeof OBSERVE_SITES[0])

/* the fire functions' network tests, `test byte [reg+0x2A44],1` then a je */
static const unsigned NET_TESTS[5] = { 0x0049D7A0u, 0x0049DA92u, 0x0049DC64u, 0x0049DE25u,
                                       0x0049DFAFu };

static int pristine(unsigned va, const unsigned char* bytes, unsigned n)
{
    return !tagpu_detour_landed(va, NULL, NULL) && memcmp(PTR(va), bytes, n) == 0;
}

void tagpu_rec_init(void)
{
    unsigned char* net[5] = { 0 };
    unsigned char* calls[START_SITE_COUNT] = { 0 };
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

    /* then all of them */
    for (i = 0; i < OBSERVE_SITE_COUNT; i++)
        if (!tagpu_detour_observe(OBSERVE_SITES[i].va, OBSERVE_SITES[i].stolen,
                                  OBSERVE_SITES[i].n, OBSERVE_SITES[i].fn, NULL)) {
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
    s_sites = 1;
    tagpu_logf("recorder: %u event sites in", (unsigned)(OBSERVE_SITE_COUNT + 5 + START_SITE_COUNT));
    return;

landfail:
    tagpu_logf("recorder: FAILED a site could not be written: the hooks that are in stay inert, "
               "and nothing will be recorded");
fail:
    for (i = 0; i < 5; i++) tagpu_detour_stub_free(net[i]);
    for (i = 0; i < START_SITE_COUNT; i++) tagpu_detour_stub_free(calls[i]);
}

/* ---- the entry points ----------------------------------------------------------------------- */

/* A level is recorded only while this file is in the game's folder at its first iteration
   (`tacli arm <instance> recorder.on`): the exact stream is too large to write for every game
   (demo-recorder-skirmish-slice.md, *Measured*). It decides whether a game is recorded, not what
   the simulation runs, so it is not a simulation lever and not in the header's lever list. */
#define REC_ON "tagpu_recorder.on"

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

    if (!ta || !level_read(ta, &L)) return;
    masks_init();
    tick = *(int*)(ta + OFF_GAMETIME);
    mode = game_mode(ta);

    if (s_state == ST_IDLE) {
        /* A recording starts at a level's first iteration or not at all, and only when its end is
           certain: the teardown wrap ends it (tagpu_rec_level_end), before the unit array is
           freed, and without the wrap nothing would. It starts only when REC_ON asks. */
        s_state = ST_DONE;
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
            else {
                s_bad = s_badlog = 0;
                InterlockedExchange(&s_offthread, 0);
                s_nano_unit = NULL;
                s_nano_out = NULL;
                s_rec = rec_start(&L, tick);
                if (s_rec) {
                    s_state = ST_RECORDING;
                    s_rec_fired = 1;
                    s_tid = s_rec->tid;
                }
            }
        }
    }
    if (++s_polls % DUMP_POLL == 0) dump_poll(&L, tick);
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
void tagpu_rec_level_end(void)
{
    s_tid = 0;
    if (s_state == ST_RECORDING && s_rec) {
        if (!s_rec->ev_full) ring_publish(s_rec, 0);
        rec_end(s_rec, s_rec->ev_full ? REC_END_RING : REC_END_TEARDOWN);
    }
    s_rec = NULL;
    s_state = ST_IDLE;
}
