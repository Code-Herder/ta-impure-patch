/* tagpu_recorder.c -- the demo recorder's capture, on the game thread
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S1; the records are tagpu_recfmt.h's).

   WHERE IT RUNS. B2's stub at the sim step's call of the unit tick (0x4954ED, the only call of
   0x48AD30) calls tagpu_rec_capture before anything else: GameTime has just been incremented
   (0x4954C0) and every phase of the previous iteration has run, so a capture sees the world
   between two iterations, on the thread that owns it. A skirmish, the only game recorded, calls
   the step with 0 (0x49693C), so the step's network pump (0x4954C8) does not run. During play the
   game thread is the only writer of the engine records and of every SIM_STATE table the capture
   reads: the loader thread's writes end before the in-play entry (0x497F40 reads the loader's
   last store, bit 1 of main+0x38D75, before the first iteration).

   WHAT IT READS, AND WHY EACH READ IS IN BOUNDS.
   - The unit array [begin, begin + nslots * 0x118): nslots is the engine's own count
     (main+0x14351), checked against the array's last-slot pointer (main+0x1435B) before any use.
     The array lives from the level load (0x4854A0) to the teardown's first free (0x485980 at
     0x491B95); the recording ends on entry to the teardown (tagpu_rec_level_end), before it.
   - The ten player records, in the main block.
   - The declared tables, each bounded by its own declaration: a static array's rows, a live
     prefix clamped to them, a heap table's counted rows. Their shadows are taken at the start,
     and grown once for a heap table that grows (table_grow).
   - The unit-type definitions' names, once, for the header: UNITINFOCount of them.
   An id row's pointer is never dereferenced: its ID is one more than its row in its table, by
   arithmetic (tagpu_recfmt.h), and a pointer outside the table becomes REC_ID_BAD, counted and
   logged.

   WHAT IT WRITES: its own memory -- the shadow images, the ring and the dump file. Nothing the
   simulation reads. */
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
#define UD_NAME        0x20         /* UnitName, 0x20 bytes                                       */
#define OFF_EACH       0x37EE6      /* u16: the units each player may have                        */
#define OFF_GAMETIME   0x38A47
#define OFF_GSTATE     0x391E9      /* GameingState*: +0 the mode (0x435100), +0x204 the TNT path  */
#define GS_TNT         0x204
#define MODE_SKIRMISH  2
#define U_STATE        0x110
#define U_ALIVE        0x10000000u
#define U_TYPE         0x92         /* the type row: its ID says whether a slot changed type      */

#define VERIFY_ROUND   30           /* iterations in which every unit slot is checked once        */
#define VERIFY_TABLE   16384        /* bytes of declared rows checked each iteration, at least a row */
#define VERIFY_CHUNK   0x7000       /* the most bytes of declared rows one verify record carries  */
#define SPAN_MAX       0x7000       /* the most image bytes one RK_TABLE_SPAN carries; a declared
                                       row wider than this is refused, since a row goes whole  */
#define SPAN_GAP       8            /* unchanged bytes a span bridges rather than splitting        */
#define DUMP_POLL      32           /* captures between two looks for the trigger                 */
#define HEADER_MAX     0xFF00       /* an RK_HEADER's body; its length is a u16                    */
#define IMG_DWORDS     (REC_PLAYER_DWORDS > REC_UNIT_DWORDS ? REC_PLAYER_DWORDS : REC_UNIT_DWORDS)

/* ---- the layout, as masks ------------------------------------------------------------------- */

/* Per image dword, the bits that are values or id bytes; per id row, where it is and its table.
   Built once from the generated table, which the build keeps equal to replay-layout.tsv. */
#define RL_ROWS (sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0])
typedef struct { unsigned short off; unsigned char table; } IdRow;
static unsigned s_umask[REC_UNIT_DWORDS], s_pmask[REC_PLAYER_DWORDS];
static IdRow    s_uid[RL_ROWS], s_pid[RL_ROWS];      /* a record's id rows are at most every row */
static unsigned s_nuid, s_npid, s_masked;

static void masks_init(void)
{
    unsigned char ub[REC_UNIT_DWORDS * 4], pb[REC_PLAYER_DWORDS * 4];
    unsigned i, k;
    if (s_masked) return;
    memset(ub, 0xFF, sizeof ub);
    memset(pb, 0xFF, sizeof pb);
    pb[REC_PLAYER_BYTES] = 0;                          /* the pad byte past the record */
    for (i = 0; i < sizeof tagpu_replay_layout / sizeof tagpu_replay_layout[0]; i++) {
        const ReplayLayoutRow* r = &tagpu_replay_layout[i];
        unsigned char* b = r->record == RL_UNIT ? ub : pb;
        if (r->cls == RL_ID) {
            IdRow* ids = r->record == RL_UNIT ? s_uid : s_pid;
            unsigned* n = r->record == RL_UNIT ? &s_nuid : &s_npid;
            ids[*n].off = r->offset;
            ids[*n].table = r->target;
            (*n)++;
            continue;
        }
        for (k = 0; k < r->size; k++)
            b[r->offset + k] &= (unsigned char)(r->mask ? ~(r->mask >> (8 * k)) : 0);
    }
    memcpy(s_umask, ub, sizeof s_umask);
    memcpy(s_pmask, pb, sizeof s_pmask);
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

static void image(const Level* L, const unsigned char* rec, unsigned bytes, const unsigned* mask,
                  unsigned dwords, const IdRow* ids, unsigned nids, unsigned* img)
{
    unsigned i;
    const char* p;
    memset(img, 0, (size_t)dwords * 4);
    memcpy(img, rec, bytes);
    for (i = 0; i < dwords; i++) img[i] &= mask[i];
    for (i = 0; i < nids; i++) {
        unsigned id;
        memcpy(&p, rec + ids[i].off, sizeof p);
        id = id_of(L, ids[i].table, p);
        if (id == REC_ID_BAD)
            bad_log(bytes == REC_UNIT_BYTES ? "a unit's" : "a player's", ids[i].off, p);
        memcpy((unsigned char*)img + ids[i].off, &id, 4);
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

static void unit_image(const Level* L, unsigned slot, unsigned* img)
{
    image(L, (const unsigned char*)L->begin + (size_t)slot * REC_UNIT_BYTES, REC_UNIT_BYTES,
          s_umask, REC_UNIT_DWORDS, s_uid, s_nuid, img);
}

static void player_image(const Level* L, unsigned i, unsigned* img)
{
    image(L, (const unsigned char*)L->ta + OFF_PLAYERS + (size_t)i * PL_STRIDE, REC_PLAYER_BYTES,
          s_pmask, REC_PLAYER_DWORDS, s_pid, s_npid, img);
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
    unsigned char* ushadow;     /* nslots images                                        */
    unsigned char* uraw;        /* nslots records as the last capture read them         */
    unsigned char* ulive;       /* nslots flags                                         */
    unsigned       pshadow[REC_PLAYERS][REC_PLAYER_DWORDS];
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

enum { ST_IDLE, ST_RECORDING, ST_DONE };        /* DONE: the rest of this level is not recorded */
static Rec*     s_rec;
static int      s_state = ST_IDLE;
static unsigned s_polls;

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

static void header_build(const Level* L, int tick, const char* map, const FILETIME* now, Tlv* t)
{
    unsigned i, h = 2166136261u, n;
    unsigned char pl[4 + PL_NAMELEN];
    const char* ta = L->ta;

    tlv_u32(t, RT_FORMAT, REC_FORMAT);
    tlv(t, RT_BUILD, GIT_COMMIT, (unsigned)strlen(GIT_COMMIT));
    tlv_u32(t, RT_MODE, MODE_SKIRMISH);
    tlv(t, RT_MAP, map, (unsigned)strlen(map));
    tlv_u32(t, RT_START, (unsigned)tick);
    tlv(t, RT_WALLCLOCK, now, 8);
    tlv_u32(t, RT_SLOTS, L->nslots);
    tlv_u32(t, RT_UNITS_EACH, *(const unsigned short*)(ta + OFF_EACH));
    for (i = 0; L->defs && i < L->ndefs; i++) {
        const char* name = L->defs + (size_t)i * UD_STRIDE + UD_NAME;
        size_t k, len = bounded_len(name, 0x20);
        for (k = 0; k < len; k++) h = (h ^ (unsigned char)name[k]) * 16777619u;
        h *= 16777619u;                                /* a zero byte ends each name */
    }
    {
        unsigned d[2];
        d[0] = L->ndefs;
        d[1] = h;
        tlv(t, RT_UNITDEFS, d, 8);
    }
    /* PlayerInfo is the 0xB9-byte block each record's constructor allocates (0x463BE0, called
       at 0x41D98B for main's eleven). Nothing frees main's: the destructor 0x463C40 runs only on a
       stack-local record (0x454538, 0x454607), and the reset keeps the pointer (0x464508). */
    for (i = 0; i < REC_PLAYERS; i++) {
        const char* rec = ta + OFF_PLAYERS + (size_t)i * PL_STRIDE;
        const unsigned char* info = *(const unsigned char* const*)(rec + PL_INFO);
        if (!rec[PL_CONTROLLER]) continue;
        pl[0] = (unsigned char)i;
        pl[1] = (unsigned char)rec[PL_CONTROLLER];
        pl[2] = info ? info[0x95] : 0xFF;
        pl[3] = info ? info[0x96] : 0xFF;
        n = (unsigned)bounded_len(rec + PL_NAME, PL_NAMELEN);
        memcpy(pl + 4, rec + PL_NAME, n);
        tlv(t, RT_PLAYER, pl, 4 + n);
    }
    for (i = 0; i < tagpu_simreg_levers(); i++) {
        int set, len;
        const char* text;
        const char* file = tagpu_simreg_lever_at(i, &set, &text, &len);
        unsigned char b[4 + 64 + 256];
        unsigned nf = (unsigned)bounded_len(file, 64), nt = text ? (unsigned)strlen(text) : 0;
        if (nt > 256) nt = 256;
        b[0] = (unsigned char)set;
        b[1] = (unsigned char)(text != NULL);
        b[2] = (unsigned char)nf;
        b[3] = 0;
        memcpy(b + 4, file, nf);
        if (nt) memcpy(b + 4 + nf, text, nt);
        tlv(t, RT_LEVER, b, 4 + nf + nt);
    }
    tlv(t, RT_LAYOUT, TAGPU_REPLAY_LAYOUT_SHA256, (unsigned)strlen(TAGPU_REPLAY_LAYOUT_SHA256));
}

static void header_tables(const Rec* r, Tlv* t)
{
    unsigned i;
    for (i = 0; i < r->ntab; i++) {
        const SimDesc* d = r->tab[i].d;
        unsigned char b[12 + 128];
        unsigned short ix = (unsigned short)r->tab[i].index, fl = d->flags;
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

/* Everything a recording will need, taken now: the capture allocates nothing after this. */
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
    r->ushadow = (unsigned char*)VirtualAlloc(NULL, (size_t)L->nslots * REC_UNIT_BYTES,
                                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->uraw = (unsigned char*)VirtualAlloc(NULL, (size_t)L->nslots * REC_UNIT_BYTES,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    r->ulive = (unsigned char*)HeapAlloc(heap, HEAP_ZERO_MEMORY, L->nslots);
    r->tab = (Table*)HeapAlloc(heap, HEAP_ZERO_MEMORY, (n ? n : 1) * sizeof *r->tab);
    if (!g->buf || !g->wake || !r->wake || !g->done || !r->done || !r->ushadow || !r->uraw ||
        !r->ulive || !r->tab)
        goto fail;
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
   (the hand-over, tagpu_recorder.h), and the writer's `done` goes to the exit's list. What an
   unfinished iteration wrote is dropped unpublished, and the ring's reserve holds the end whatever
   the published part left. */
static void rec_end(Rec* r, unsigned reason)
{
    unsigned char b[12], * at;
    unsigned short why = (unsigned short)reason, z = 0;
    unsigned pos;
    r->w = (unsigned)InterlockedCompareExchange(&r->ring->wpub, 0, 0);
    memcpy(b, &why, 2);
    memcpy(b + 2, &z, 2);
    memcpy(b + 4, &s_bad, 4);
    memcpy(b + 8, &r->high, 4);
    pos = r->w & (REC_RING_BYTES - 1);
    at = r->ring->buf + pos;
    if (pos + 16 > REC_RING_BYTES) {
        RecHdr h = { RK_PAD, 0 };
        memcpy(at, &h, 4);
        r->w += REC_RING_BYTES - pos;
        at = r->ring->buf;
    }
    memcpy(at + 4, b, 12);
    ring_commit(r, at, RK_END, 12);
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
    tagpu_logf("recorder: ended after GameTime %d: %s; %u id rows outside their tables", r->tick,
               reason == REC_END_TEARDOWN ? "the level ended" :
               reason == REC_END_RING     ? "INCOMPLETE, the ring was full" :
               reason == REC_END_WRITE    ? "INCOMPLETE, the file could not be written" :
               reason == REC_END_TABLE    ? "INCOMPLETE, no memory for a declared table's growth" :
               reason == REC_END_LEVEL    ? "INCOMPLETE, the level changed without a teardown" :
                                            "INCOMPLETE, the game stopped being a skirmish", s_bad);
    rec_free(r);
}

/* ---- one iteration -------------------------------------------------------------------------- */

/* Every slot: a unit that died goes, a unit whose record's bytes moved is imaged, and its image's
   changed dwords go into the ring. A record whose bytes did not move keeps the image it had: an
   image is a function of its record's bytes alone within a recording, since the tables its IDs
   index -- the unit array, the defs, the players, the weapons, the buckets -- do not move until
   the level ends, which ends the recording. */
static int cap_units(Rec* r, const Level* L)
{
    unsigned slot, img[REC_UNIT_DWORDS];
    for (slot = 1; slot < L->nslots; slot++) {
        unsigned* sh = (unsigned*)(r->ushadow + (size_t)slot * REC_UNIT_BYTES);
        const unsigned char* rec = (const unsigned char*)L->begin + (size_t)slot * REC_UNIT_BYTES;
        unsigned char* raw = r->uraw + (size_t)slot * REC_UNIT_BYTES;
        if (!unit_live(L, slot)) {
            if (r->ulive[slot]) {
                unsigned short pre[2] = { (unsigned short)slot, 0 };
                if (!ring_put(r, RK_UNIT_GONE, pre, 4, NULL, 0)) return 0;
                r->ulive[slot] = 0;
            }
            continue;
        }
        if (r->ulive[slot] && diff_from(rec, raw, 0, REC_UNIT_BYTES) == REC_UNIT_BYTES) continue;
        memcpy(raw, rec, REC_UNIT_BYTES);
        unit_image(L, slot, img);
        /* a new unit, or a slot whose type changed, goes whole: nothing of its past applies */
        if (!r->ulive[slot] ||
            memcmp((unsigned char*)img + U_TYPE, (unsigned char*)sh + U_TYPE, 4)) {
            unsigned short pre[2] = { (unsigned short)slot, 0 };
            if (!ring_put(r, RK_UNIT, pre, 4, img, REC_UNIT_BYTES)) return 0;
        } else {
            unsigned char bits[REC_UNIT_BITS + 1] = { 0 }, * at;
            unsigned short s = (unsigned short)slot;
            unsigned i, k = 0, n = 0;
            for (i = 0; i < REC_UNIT_DWORDS; i++)
                if (img[i] != sh[i]) { bits[i >> 3] |= (unsigned char)(1u << (i & 7)); n++; }
            if (!n) continue;
            at = ring_take(r, 12 + 4 * n);
            if (!at) return 0;
            memcpy(at + 4, &s, 2);
            memcpy(at + 6, bits, REC_UNIT_BITS + 1);
            for (i = 0; i < REC_UNIT_DWORDS; i++)
                if (img[i] != sh[i]) memcpy(at + 16 + 4 * k++, &img[i], 4);
            ring_commit(r, at, RK_UNIT_DELTA, 12 + 4 * n);
        }
        memcpy(sh, img, REC_UNIT_BYTES);
        r->ulive[slot] = 1;
    }
    return 1;
}

static int cap_players(Rec* r, const Level* L)
{
    unsigned i, k, img[REC_PLAYER_DWORDS];
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned* sh = r->pshadow[i];
        player_image(L, i, img);
        if (r->first) {
            unsigned short pre[2] = { (unsigned short)i, 0 };
            if (!ring_put(r, RK_PLAYER, pre, 4, img, sizeof img)) return 0;
        } else {
            unsigned char bits[REC_PLAYER_BITS + 3] = { 0 }, * at;
            unsigned short s = (unsigned short)i;
            unsigned j = 0, n = 0;
            for (k = 0; k < REC_PLAYER_DWORDS; k++)
                if (img[k] != sh[k]) { bits[k >> 3] |= (unsigned char)(1u << (k & 7)); n++; }
            if (!n) continue;
            at = ring_take(r, 16 + 4 * n);
            if (!at) return 0;
            memcpy(at + 4, &s, 2);
            memcpy(at + 6, bits, REC_PLAYER_BITS + 3);
            for (k = 0; k < REC_PLAYER_DWORDS; k++)
                if (img[k] != sh[k]) memcpy(at + 20 + 4 * j++, &img[k], 4);
            ring_commit(r, at, RK_PLAYER_DELTA, 16 + 4 * n);
        }
        memcpy(sh, img, sizeof img);
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
   slots, one player and the next VERIFY_TABLE bytes of the declared tables' rows, so that every
   unit slot is checked once every VERIFY_ROUND iterations and every row in turn. The writer
   compares them with its own copy, which only the ring has built. */
static int cap_verify(Rec* r, const Level* L)
{
    unsigned k, per = (L->nslots - 1 + VERIFY_ROUND - 1) / VERIFY_ROUND, total = 0, budget, skips;
    unsigned img[IMG_DWORDS];
    for (k = 0; k < per; k++) {
        unsigned short pre[2];
        int live;
        if (++r->vslot >= L->nslots) r->vslot = 1;
        live = unit_live(L, r->vslot);
        pre[0] = (unsigned short)r->vslot;
        pre[1] = (unsigned short)live;
        if (live) unit_image(L, r->vslot, img);
        if (!ring_put(r, RK_VERIFY_UNIT, pre, 4, img, live ? REC_UNIT_BYTES : 0)) return 0;
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

/* The live world's images as a stored RB_SNAPSHOT: what the recording must read back to at this
   GameTime. Written on the game thread, which is tooling's price: a dump is the harness's. */
static void dump_write(const Level* L, int tick, const char* name)
{
    char path[MAX_PATH];
    RecBlock b;
    unsigned slot, i, img[IMG_DWORDS], n = tagpu_simreg_count();
    static unsigned char rb[SPAN_MAX];
    const SimDesc* wide = table_too_wide();
    if (wide) {
        tagpu_logf("recorder: dump %s refused: %s has rows past %u bytes", name, wide->name,
                   SPAN_MAX);
        return;
    }
    CreateDirectoryA("replays", NULL);
    snprintf(path, sizeof path, "replays\\dump-%s.bin", name);
    s_dump = fopen(path, "wb");
    if (!s_dump) { tagpu_logf("recorder: dump %s: cannot write %s", name, path); return; }
    fwrite(REC_MAGIC, 1, 8, s_dump);
    memset(&b, 0, sizeof b);
    fwrite(&b, sizeof b, 1, s_dump);                  /* rewritten once the body is known */
    s_dump_raw = 0;
    s_dump_crc = 0;
    dump_rec(RK_TICK, &tick, 4, NULL, 0);
    for (slot = 1; slot < L->nslots; slot++) {
        unsigned short pre[2] = { (unsigned short)slot, 0 };
        if (!unit_live(L, slot)) continue;
        unit_image(L, slot, img);
        dump_rec(RK_UNIT, pre, 4, img, REC_UNIT_BYTES);
    }
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned short pre[2] = { (unsigned short)i, 0 };
        player_image(L, i, img);
        dump_rec(RK_PLAYER, pre, 4, img, REC_PLAYER_DWORDS * 4);
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
    fseek(s_dump, 8, SEEK_SET);
    fwrite(&b, sizeof b, 1, s_dump);
    fclose(s_dump);
    s_dump = NULL;
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

/* ---- the entry points ----------------------------------------------------------------------- */

/* A level is recorded only while this file is in the game's folder at its first iteration
   (`tacli arm <instance> recorder.on`): the exact stream is too large to write for every game
   (demo-recorder-skirmish-slice.md, *Measured*). It decides whether a game is recorded, not what
   the simulation runs, so it is not a simulation lever and not in the header's lever list. */
#define REC_ON "tagpu_recorder.on"

static void rec_stop(unsigned reason)
{
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
            if (!tagpu_opt_on(REC_ON))
                tagpu_logf("recorder: not recording: %s is absent", REC_ON);
            else if (!tagpu_reclaim_level_tracked())
                tagpu_logf("recorder: not recording: the teardown wrap is not in (tagpu_reclaim.c), "
                           "so nothing would end a recording");
            else if (!tagpu_simreg_count())
                tagpu_logf("recorder: not recording: the sim-state registry accepted nothing");
            else {
                s_bad = s_badlog = 0;
                s_rec = rec_start(&L, tick);
                if (s_rec) s_state = ST_RECORDING;
            }
        }
    }
    if (++s_polls % DUMP_POLL == 0) dump_poll(&L, tick);
    if (s_state != ST_RECORDING) return;

    r = s_rec;
    if (InterlockedCompareExchange(&r->ring->failed, 0, 0)) { rec_stop(REC_END_WRITE); return; }
    if (mode != MODE_SKIRMISH) { rec_stop(REC_END_MODE); return; }
    if (L.begin != r->begin || L.nslots != r->nslots || tick <= r->tick) {
        rec_stop(REC_END_LEVEL);
        return;
    }
    QueryPerformanceCounter(&t0);
    w0 = r->w;
    ok = ring_put(r, RK_TICK, &tick, 4, NULL, 0) && cap_units(r, &L) && cap_players(r, &L);
    t = ok ? cap_tables(r, &L) : 0;
    if (t < 0) { r->w = w0; rec_stop(REC_END_TABLE); return; }
    if (!t || !cap_verify(r, &L)) {
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

void tagpu_rec_level_end(void)
{
    if (s_state == ST_RECORDING && s_rec) rec_end(s_rec, REC_END_TEARDOWN);
    s_rec = NULL;
    s_state = ST_IDLE;
}
