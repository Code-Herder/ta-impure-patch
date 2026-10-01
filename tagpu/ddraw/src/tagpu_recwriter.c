/* tagpu_recwriter.c -- the demo recorder's writer thread
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S1 and S2a; the records are
   tagpu_recfmt.h's).

   It reads no engine memory. Everything it knows arrives through the ring (tagpu_recorder.h), and
   this file is not on thread-split.allow, so the build refuses an engine address here. It keeps a
   copy of the recorded world built from the records alone, compares it with the fresh images the
   capture sends each iteration, and writes a difference into the file as a correction, counted and
   logged. The file is blocks (RecBlock): the header, stream blocks of whole iterations, a snapshot
   of the copy every SNAPSHOT_TICKS, and the end.

   A record is checked against the copy's bounds before it is applied: a slot, a player, a table
   index, an offset and a length, and an event's length. One that fails is a fault in the capture,
   not in the game: it abandons the file (the ring's `failed`, which ends the recording at the next
   capture) and is logged. The writer then only waits for the hand-over, and parses nothing more.
   The events pass into the stream as they came: the copy is the images', and an event changes
   none of it -- the cells inside the feature tick's event included, which are its input.

   THE FEATURES (the slice note's S3a) are in the copy as the capture sends them, with no verify of
   their own: each iteration the capture compares every cell a writer marked and every record of the
   wreck pool with its own copy and sends what changed (tagpu_recorder.c, *The features*). The
   grid's first image comes through the ring's `grid`, larger than the ring on the largest maps:
   RK_GRID says it is there, and the copy is that buffer from then on.

   THE GLOBALS AND THE PROJECTILES (S3b) are in the copy as well, and checked like the units: the
   globals image whole once each rotation, a slice of the pool's records with its count and extent
   each iteration.

   A SNAPSHOT is smaller than the copy: its cells are the ones that differ from the first
   iteration's grid, the base, which the writer keeps from RK_GRID on and a reader takes from the
   first iteration it reads; and a table's spans that are all zero are left out, since a snapshot
   builds from nothing and RK_TABLE_SIZE zeroes the table. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_recorder.h"
#include "tagpu_recfmt.h"
#include "tagpu_log.h"
#include "crc32.h"
#include "lodepng.h"

#define STREAM_BYTES   (256u << 10)  /* a stream block ends at an iteration past this many bytes */
#define STREAM_TICKS   300           /* ... or this many iterations, whichever comes first         */
#define SNAPSHOT_TICKS 900           /* 30 s of game time: a placeholder S3 measures (the plan)     */
#define SPAN_CHUNK     0x7000        /* the most bytes one of a snapshot's spans carries            */
#define CORR_MAX       256           /* distinct (kind, table, offset) correction counters          */
#define CORR_LOG       16            /* corrections logged, a recording                             */
#define UNIT_LIVE_OFF  REC_UNIT_IMAGE /* a correction counter's offset for a unit's liveness        */

typedef struct Buf { unsigned char* p; size_t n, cap; int oom; } Buf;

/* an order list's images, in order */
typedef struct WList { unsigned n, cap; unsigned char* img; } WList;

/* A list whose RK_ORDERS (or RK_VERIFY_ORDERS) records have begun: the next ones must be its own,
   `first` ascending, until all `total` nodes have come. */
typedef struct Pending { unsigned kind, slot, list, total, next; } Pending;

typedef struct WTable {
    unsigned       index;       /* the registry's, as the records name it */
    unsigned       stride;
    unsigned       size, cap;   /* bytes of image, bytes allocated        */
    unsigned char* img;
    char           name[64];
} WTable;

typedef struct Corr { unsigned kind, table, off, count; } Corr;

typedef struct Writer {
    RecRing*       ring;
    HANDLE         file;
    char           path[MAX_PATH];
    int            header;      /* RK_HEADER has been applied                       */
    int            dead;        /* nothing more is written or parsed                */

    unsigned       nslots;      /* the copy                                         */
    unsigned char* units;
    unsigned char* live;
    WList*         lists;       /* nslots * 2: main, then sub                       */
    unsigned       players[REC_PLAYERS][REC_PLAYER_DWORDS];
    unsigned char  ctl[REC_PLAYERS], view[2];
    WTable         tab[REC_TABLE_MAX];
    unsigned       ntab;
    Pending        pend;        /* pend.total > pend.next while a list is arriving  */
    unsigned char* vlist;       /* a verified list, as it arrives                   */

    Buf            stream;      /* the stream block being built, whole iterations   */
    unsigned       sfirst;      /* its first GameTime                               */
    unsigned       sticks;      /* its iterations                                   */
    Buf            snap;        /* reused for each snapshot                         */
    unsigned       tick;        /* the iteration being applied                      */
    int            ticked;      /* an iteration has begun                           */
    unsigned       snap_tick;   /* the last snapshot's GameTime, or the first tick  */
    unsigned       iterations;

    Corr           corr[CORR_MAX];
    unsigned       ncorr, corr_more, logged;

    unsigned       gw, gh, npool;   /* the features' copy (RT_FEATURES)                 */
    unsigned char* grid;        /* the ring's `grid`, once RK_GRID has come               */
    unsigned char* pool;
    int32_t        heads[4];    /* the pool's heads and the cursor                        */
    int            heads_in;
    unsigned       fbracket;    /* an RK_EV_FEATURE's root + 1, until its END             */
    unsigned       ndeaths;     /* the deaths whose RK_EV_DEATH_END is still to come      */
    unsigned       death[REC_DEATHS_MAX];   /* their units' IDs, innermost last          */

    unsigned char* base;        /* the grid at the first iteration (a snapshot's cells differ) */
    unsigned       globals[REC_GLOBALS_DWORDS];   /* the globals image, once sent        */
    int            globals_in;
    unsigned       ncap;        /* the projectile pool's records (RT_PROJECTILES)          */
    unsigned char* proj;        /* their images                                           */
    unsigned       pcount, phw; /* the count and the written extent, once sent:          */
    int            phead_in;    /*   records [phw, ncap) are zero                         */
    unsigned char* mapped;      /* the explored map, REC_MAPPED_BYTES of the grid          */
    unsigned       mbytes;
    unsigned       mnext;       /* RK_MAPPED's next offset: mbytes once the map is whole  */

    unsigned       end_reason, bad_ids, ring_high, off_thread, uncredited, unobserved;
    unsigned long long raw, packed;   /* the stream's bytes, before and after deflate */
} Writer;

/* ---- buffers -------------------------------------------------------------------------------- */

static int buf_put(Buf* b, const void* p, size_t n)
{
    if (b->oom) return 0;
    if (!n) return 1;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : (1u << 20);
        unsigned char* q;
        while (cap < b->n + n) cap *= 2;
        q = (unsigned char*)realloc(b->p, cap);
        if (!q) { b->oom = 1; return 0; }
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
    return 1;
}

static void buf_rec(Buf* b, unsigned kind, const void* pre, unsigned npre, const void* pay,
                    unsigned npay)
{
    static const unsigned char zero[4];
    unsigned body = npre + npay;
    RecHdr h;
    h.kind = (uint16_t)kind;
    h.len = (uint16_t)body;
    buf_put(b, &h, 4);
    buf_put(b, pre, npre);
    buf_put(b, pay, npay);
    buf_put(b, zero, ((4 + body + 3) & ~3u) - 4 - body);
}

/* ---- the file ------------------------------------------------------------------------------- */

static void fail(Writer* w, const char* why)
{
    if (w->dead) return;
    w->dead = 1;
    InterlockedExchange(&w->ring->failed, 1);
    tagpu_logf("recorder: writer: %s: %s is abandoned at its last complete block", why, w->path);
    if (w->file != INVALID_HANDLE_VALUE) CloseHandle(w->file);
    w->file = INVALID_HANDLE_VALUE;
}

static void put(Writer* w, const void* p, DWORD n)
{
    DWORD done = 0;
    if (w->dead) return;
    if (!WriteFile(w->file, p, n, &done, NULL) || done != n) {
        char why[64];
        snprintf(why, sizeof why, "cannot write (%lu)", GetLastError());
        fail(w, why);
    }
}

/* One block: deflated when that is smaller, else stored. */
static void block(Writer* w, unsigned kind, unsigned first, unsigned last, const unsigned char* raw,
                  size_t n, int pack)
{
    RecBlock b;
    unsigned char* z = NULL;
    size_t zn = 0;
    const unsigned char* body = raw;
    size_t bn = n;
    if (w->dead) return;
    memset(&b, 0, sizeof b);
    b.magic = REC_BLOCK_MAGIC;
    b.kind = (uint16_t)kind;
    b.first = first;
    b.last = last;
    b.raw = (uint32_t)n;
    if (pack && n && !lodepng_zlib_compress(&z, &zn, raw, n, &lodepng_default_compress_settings)
        && zn < n) {
        body = z;
        bn = zn;
    } else {
        b.flags = RBF_STORED;
    }
    b.packed = (uint32_t)bn;
    b.crc_body = (uint32_t)Crc32_ComputeBuf(0, body, bn);
    b.crc_head = (uint32_t)Crc32_ComputeBuf(0, &b, 28);
    put(w, &b, sizeof b);
    put(w, body, (DWORD)bn);
    if (kind == RB_STREAM) {
        w->raw += n;
        w->packed += bn;
    }
    free(z);
}

/* the path the capture chose, or its first free " (n)" variant */
static int file_open(Writer* w)
{
    const char* dot = strrchr(w->ring->path, '.');
    int k;
    CreateDirectoryA("replays", NULL);
    for (k = 1; k < 10; k++) {
        if (k == 1) snprintf(w->path, sizeof w->path, "%s", w->ring->path);
        else snprintf(w->path, sizeof w->path, "%.*s (%d)%s", (int)(dot - w->ring->path),
                      w->ring->path, k, dot);
        w->file = CreateFileA(w->path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
        if (w->file != INVALID_HANDLE_VALUE) return 1;
        if (GetLastError() != ERROR_FILE_EXISTS || !dot) break;
    }
    snprintf(w->path, sizeof w->path, "%s", w->ring->path);
    fail(w, "cannot create the file");
    return 0;
}

/* ---- the copy ------------------------------------------------------------------------------- */

static WTable* table_of(Writer* w, unsigned index)
{
    unsigned i;
    for (i = 0; i < w->ntab; i++)
        if (w->tab[i].index == index) return &w->tab[i];
    return NULL;
}

static int table_resize(WTable* t, unsigned bytes)
{
    if (bytes > t->cap) {
        unsigned char* n = (unsigned char*)realloc(t->img, bytes);
        if (!n) return 0;
        t->img = n;
        t->cap = bytes;
    }
    if (bytes > t->size) memset(t->img + t->size, 0, bytes - t->size);
    t->size = bytes;
    return 1;
}

static int slot_ok(const Writer* w, unsigned slot) { return slot >= 1 && slot < w->nslots; }

/* the dwords a delta names: its bitmap's set bits, none past `dwords` */
static int delta_count(const unsigned char* bits, unsigned nbytes, unsigned dwords, unsigned* n)
{
    unsigned i, c = 0;
    for (i = 0; i < nbytes * 8; i++)
        if (bits[i >> 3] >> (i & 7) & 1) {
            if (i >= dwords) return 0;
            c++;
        }
    *n = c;
    return 1;
}

static void delta_apply(unsigned* img, const unsigned char* bits, unsigned dwords,
                        const unsigned char* vals)
{
    unsigned i, k = 0;
    for (i = 0; i < dwords; i++)
        if (bits[i >> 3] >> (i & 7) & 1) memcpy(&img[i], vals + 4 * k++, 4);
}

/* ---- the header ----------------------------------------------------------------------------- */

static int header_apply(Writer* w, const unsigned char* b, unsigned len)
{
    unsigned at = 0;
    while (at + 4 <= len) {
        unsigned short tag, n;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) return 0;
        if (tag == RT_SLOTS && n == 4) memcpy(&w->nslots, b + at + 4, 4);
        if (tag == RT_PLAYER && n >= 2 && b[at + 4] < REC_PLAYERS) w->ctl[b[at + 4]] = b[at + 5];
        if (tag == RT_OPTIONS && n == 20) {
            w->view[0] = b[at + 4 + 18];
            w->view[1] = b[at + 4 + 19];
        }
        if (tag == RT_FEATURES && n == 20) {
            memcpy(&w->gw, b + at + 4, 4);
            memcpy(&w->gh, b + at + 8, 4);
            memcpy(&w->npool, b + at + 12, 4);
        }
        if (tag == RT_PROJECTILES && n == 4) memcpy(&w->ncap, b + at + 4, 4);
        if (tag == RT_TABLE && n >= 16) {
            WTable* t;
            unsigned short ix;
            unsigned stride, nn = n - 16;
            if (w->ntab >= REC_TABLE_MAX) return 0;
            memcpy(&ix, b + at + 4, 2);
            memcpy(&stride, b + at + 8, 4);
            if (!stride || stride > SPAN_CHUNK || table_of(w, ix)) return 0;
            t = &w->tab[w->ntab++];
            t->index = ix;
            t->stride = stride;
            if (nn >= sizeof t->name) nn = sizeof t->name - 1;
            memcpy(t->name, b + at + 20, nn);
            t->name[nn] = 0;
        }
        at += (4 + n + 3) & ~3u;
    }
    if (w->nslots < 2 || w->nslots > 0xFFFF || !w->gw || w->gw > 0xFFFF || !w->gh ||
        w->gh > 0xFFFF || !w->npool || w->npool > 0x8000 || !w->ncap || w->ncap > 0xFFFF)
        return 0;
    w->units = (unsigned char*)VirtualAlloc(NULL, (size_t)w->nslots * REC_UNIT_IMAGE,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    w->live = (unsigned char*)calloc(w->nslots, 1);
    w->lists = (WList*)calloc((size_t)w->nslots * 2, sizeof *w->lists);
    w->vlist = (unsigned char*)malloc((size_t)REC_ORDERS_MAX * REC_ORDER_IMAGE);
    w->pool = (unsigned char*)calloc(w->npool, REC_WRECK_BYTES);
    w->proj = (unsigned char*)calloc(w->ncap, REC_PROJ_IMAGE);
    w->mbytes = REC_MAPPED_BYTES(w->gw, w->gh);
    w->mapped = (unsigned char*)malloc(w->mbytes);
    if (!w->units || !w->live || !w->lists || !w->vlist || !w->pool || !w->proj || !w->mapped) {
        fail(w, "no memory for the copy");
        return 1;
    }
    if (!file_open(w)) return 1;
    put(w, REC_MAGIC, 8);
    block(w, RB_HEADER, 0, 0, b, len, 0);
    tagpu_logf("recorder: writer: %s: %u slots, %u tables", w->path, w->nslots, w->ntab);
    return 1;
}

/* ---- corrections ---------------------------------------------------------------------------- */

static void corr_count(Writer* w, unsigned kind, unsigned table, unsigned off)
{
    unsigned i;
    for (i = 0; i < w->ncorr; i++)
        if (w->corr[i].kind == kind && w->corr[i].table == table && w->corr[i].off == off) {
            w->corr[i].count++;
            return;
        }
    if (w->ncorr < CORR_MAX) {
        Corr* c = &w->corr[w->ncorr++];
        c->kind = kind;
        c->table = table;
        c->off = off;
        c->count = 1;
    } else {
        w->corr_more++;
    }
}

static void corr_log(Writer* w, const char* what, unsigned id, unsigned off, unsigned built,
                     unsigned live)
{
    if (w->logged >= CORR_LOG) return;
    w->logged++;
    tagpu_logf("recorder: writer: correction at GameTime %u: %s %u +0x%X is 0x%08X, the stream "
               "built 0x%08X", w->tick, what, id, off, live, built);
}

static void list_clear(Writer* w, unsigned slot)
{
    w->lists[slot * 2].n = 0;
    w->lists[slot * 2 + 1].n = 0;
}

static int list_resize(WList* l, unsigned n)
{
    if (n > l->cap) {
        unsigned char* q = (unsigned char*)realloc(l->img, (size_t)n * REC_ORDER_IMAGE);
        if (!q) return 0;
        l->img = q;
        l->cap = n;
    }
    l->n = n;
    return 1;
}

/* a list's records, as the capture's olist_put makes them */
static void list_put(Buf* b, unsigned kind, unsigned slot, unsigned list, const unsigned char* img,
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
        buf_rec(b, kind, pre, 8, img + (size_t)first * REC_ORDER_IMAGE, k * REC_ORDER_IMAGE);
        first += k;
    } while (first < n);
}

static void verify_unit(Writer* w, unsigned slot, unsigned live, const unsigned char* img)
{
    unsigned char* have = w->units + (size_t)slot * REC_UNIT_IMAGE;
    unsigned short pre[2] = { (unsigned short)slot, 0 };
    unsigned corr0 = 0, i;
    if (live != w->live[slot]) {
        corr_count(w, RK_UNIT, 0, UNIT_LIVE_OFF);
        corr_log(w, "unit", slot, UNIT_LIVE_OFF, w->live[slot], live);
    } else if (live && memcmp(have, img, REC_UNIT_IMAGE)) {
        for (i = 0; i < REC_UNIT_IMAGE; i += 4) {
            unsigned a, b;
            memcpy(&a, have + i, 4);
            memcpy(&b, img + i, 4);
            if (a == b) continue;
            corr_count(w, RK_UNIT, 0, i);
            corr_log(w, "unit", slot, i, a, b);
        }
    } else {
        return;
    }
    buf_rec(&w->stream, RK_CORRECT, &corr0, 4, NULL, 0);
    if (live) {
        memcpy(have, img, REC_UNIT_IMAGE);
        buf_rec(&w->stream, RK_UNIT, pre, 4, img, REC_UNIT_IMAGE);
    } else {
        buf_rec(&w->stream, RK_UNIT_GONE, pre, 4, NULL, 0);
        list_clear(w, slot);
    }
    w->live[slot] = (unsigned char)live;
}

/* A verified list has come whole: the copy's must equal it, node by node and in length. A
   difference is corrected by the whole list, as the capture sends one. */
static void verify_list(Writer* w, unsigned slot, unsigned list, const unsigned char* img,
                        unsigned n)
{
    WList* l = &w->lists[slot * 2 + list];
    unsigned corr0 = 0, i, k = n < l->n ? n : l->n;
    if (n == l->n && (!n || !memcmp(l->img, img, (size_t)n * REC_ORDER_IMAGE))) return;
    if (n != l->n) {
        corr_count(w, RK_ORDERS, list, REC_ORDERS_LENGTH);
        corr_log(w, list ? "sub list" : "main list", slot, REC_ORDERS_LENGTH, l->n, n);
    }
    for (i = 0; i < k * REC_ORDER_IMAGE; i += 4) {
        unsigned a, b;
        memcpy(&a, l->img + i, 4);
        memcpy(&b, img + i, 4);
        if (a == b) continue;
        corr_count(w, RK_ORDERS, list, i % REC_ORDER_IMAGE);
        corr_log(w, list ? "sub list" : "main list", slot, i, a, b);
    }
    if (!list_resize(l, n)) { fail(w, "no memory for an order list's copy"); return; }
    memcpy(l->img, img, (size_t)n * REC_ORDER_IMAGE);
    buf_rec(&w->stream, RK_CORRECT, &corr0, 4, NULL, 0);
    list_put(&w->stream, RK_ORDERS, slot, list, img, n);
}

static void verify_player(Writer* w, unsigned i, const unsigned char* img)
{
    unsigned short pre[2] = { (unsigned short)i, 0 };
    unsigned corr0 = 0, k;
    if (!memcmp(w->players[i], img, REC_PLAYER_DWORDS * 4)) return;
    for (k = 0; k < REC_PLAYER_DWORDS; k++) {
        unsigned b;
        memcpy(&b, img + 4 * k, 4);
        if (w->players[i][k] == b) continue;
        corr_count(w, RK_PLAYER, 0, 4 * k);
        corr_log(w, "player", i, 4 * k, w->players[i][k], b);
    }
    memcpy(w->players[i], img, REC_PLAYER_DWORDS * 4);
    buf_rec(&w->stream, RK_CORRECT, &corr0, 4, NULL, 0);
    buf_rec(&w->stream, RK_PLAYER, pre, 4, img, REC_PLAYER_DWORDS * 4);
}

static void verify_table(Writer* w, WTable* t, unsigned off, const unsigned char* img, unsigned n)
{
    unsigned char pre[8];
    unsigned short ix = (unsigned short)t->index, z = 0;
    unsigned corr0 = 0, i;
    if (!memcmp(t->img + off, img, n)) return;
    for (i = 0; i < n; i += 4) {
        unsigned a = 0, b = 0, m = n - i < 4 ? n - i : 4;
        memcpy(&a, t->img + off + i, m);
        memcpy(&b, img + i, m);
        if (a == b) continue;
        corr_count(w, RK_TABLE_SPAN, t->index, off + i);
        corr_log(w, t->name, (off + i) / t->stride, (off + i) % t->stride, a, b);
    }
    memcpy(t->img + off, img, n);
    memcpy(pre, &ix, 2);
    memcpy(pre + 2, &z, 2);
    memcpy(pre + 4, &off, 4);
    buf_rec(&w->stream, RK_CORRECT, &corr0, 4, NULL, 0);
    buf_rec(&w->stream, RK_TABLE_SPAN, pre, 8, img, n);
}

static void verify_globals(Writer* w, const unsigned char* img)
{
    unsigned corr0 = 0, i;
    if (!memcmp(w->globals, img, sizeof w->globals)) return;
    for (i = 0; i < REC_GLOBALS_DWORDS; i++) {
        unsigned b;
        memcpy(&b, img + 4 * i, 4);
        if (w->globals[i] == b) continue;
        corr_count(w, RK_GLOBALS, 0, 4 * i);
        corr_log(w, "globals dword", i, 4 * i, w->globals[i], b);
    }
    memcpy(w->globals, img, sizeof w->globals);
    buf_rec(&w->stream, RK_CORRECT, &corr0, 4, NULL, 0);
    buf_rec(&w->stream, RK_GLOBALS, w->globals, sizeof w->globals, NULL, 0);
}

/* the pool's count and extent; the records an extent gives up are zero again */
static void proj_head(Writer* w, unsigned count, unsigned hw)
{
    if (w->phead_in && hw < w->phw)
        memset(w->proj + (size_t)hw * REC_PROJ_IMAGE, 0, (size_t)(w->phw - hw) * REC_PROJ_IMAGE);
    w->pcount = count;
    w->phw = hw;
    w->phead_in = 1;
}

/* The count, the extent and the records [first, first + n) read afresh: the copy's must equal
   them. A difference is corrected by the head, or by the whole record, as the capture sends one. */
static void verify_proj(Writer* w, unsigned count, unsigned hw, unsigned first,
                        const unsigned char* img, unsigned n)
{
    unsigned corr0 = 0, i, k;
    if (!w->phead_in || count != w->pcount || hw != w->phw) {
        unsigned head[2] = { count, hw };
        if (count != w->pcount) {
            corr_count(w, RK_PROJ_HEAD, 0, 0);
            corr_log(w, "projectile count", 0, 0, w->pcount, count);
        }
        if (hw != w->phw) {
            corr_count(w, RK_PROJ_HEAD, 0, 4);
            corr_log(w, "projectile extent", 0, 4, w->phw, hw);
        }
        proj_head(w, count, hw);
        buf_rec(&w->stream, RK_CORRECT, &corr0, 4, NULL, 0);
        buf_rec(&w->stream, RK_PROJ_HEAD, head, sizeof head, NULL, 0);
    }
    for (k = 0; k < n; k++) {
        unsigned char* have = w->proj + (size_t)(first + k) * REC_PROJ_IMAGE;
        const unsigned char* live = img + (size_t)k * REC_PROJ_IMAGE;
        unsigned short pre[2] = { (unsigned short)(first + k), 0 };
        if (!memcmp(have, live, REC_PROJ_IMAGE)) continue;
        for (i = 0; i < REC_PROJ_IMAGE; i += 4) {
            unsigned a, b;
            memcpy(&a, have + i, 4);
            memcpy(&b, live + i, 4);
            if (a == b) continue;
            corr_count(w, RK_PROJ, 0, i);
            corr_log(w, "projectile", first + k, i, a, b);
        }
        memcpy(have, live, REC_PROJ_IMAGE);
        buf_rec(&w->stream, RK_CORRECT, &corr0, 4, NULL, 0);
        buf_rec(&w->stream, RK_PROJ, pre, 4, live, REC_PROJ_IMAGE);
    }
}

/* every cell of the grid as RK_CELLS, a row in runs of at most REC_CELLS_MAX */
static void grid_put(Buf* b, const Writer* w)
{
    unsigned x, z;
    for (z = 0; z < w->gh; z++)
        for (x = 0; x < w->gw; ) {
            unsigned n = w->gw - x < REC_CELLS_MAX ? w->gw - x : REC_CELLS_MAX;
            unsigned short pre[4];
            pre[0] = (unsigned short)x;
            pre[1] = (unsigned short)z;
            pre[2] = (unsigned short)n;
            pre[3] = 0;
            buf_rec(b, RK_CELLS, pre, 8, w->grid + ((size_t)z * w->gw + x) * REC_CELL_BYTES,
                    n * REC_CELL_BYTES);
            x += n;
        }
}

/* the cells that differ from the base as RK_CELLS, a run of them at most REC_CELLS_MAX */
static void grid_diff_put(Buf* b, const Writer* w)
{
    size_t row = (size_t)w->gw * REC_CELL_BYTES;
    unsigned x, z;
    for (z = 0; z < w->gh; z++) {
        const unsigned char* g = w->grid + z * row;
        const unsigned char* o = w->base + z * row;
        if (!memcmp(g, o, row)) continue;
        for (x = 0; x < w->gw; ) {
            unsigned n = 0;
            unsigned short pre[4];
            while (x + n < w->gw && n < REC_CELLS_MAX &&
                   memcmp(g + (size_t)(x + n) * REC_CELL_BYTES, o + (size_t)(x + n) * REC_CELL_BYTES,
                          REC_CELL_BYTES))
                n++;
            if (!n) { x++; continue; }
            pre[0] = (unsigned short)x;
            pre[1] = (unsigned short)z;
            pre[2] = (unsigned short)n;
            pre[3] = 0;
            buf_rec(b, RK_CELLS, pre, 8, g + (size_t)x * REC_CELL_BYTES, n * REC_CELL_BYTES);
            x += n;
        }
    }
}

/* 1 when the `n` bytes at `p` are all zero */
static int all_zero(const unsigned char* p, unsigned n)
{
    while (n && !*p) { p++; n--; }
    return !n;
}

/* ---- blocks of the stream ------------------------------------------------------------------- */

static void stream_flush(Writer* w)
{
    if (!w->stream.n) return;
    if (w->stream.oom) { fail(w, "no memory for a stream block"); return; }
    block(w, RB_STREAM, w->sfirst, w->tick, w->stream.p, w->stream.n, 1);
    w->stream.n = 0;
    w->sticks = 0;
}

/* The copy as records that build it, at the iteration just completed: from nothing but the base
   grid, which every reader holds before it reads a snapshot (tagpu_recfmt.h, RB_SNAPSHOT). */
static void snapshot(Writer* w)
{
    Buf* b = &w->snap;
    unsigned s, i, off;
    b->n = 0;
    buf_rec(b, RK_TICK, &w->tick, 4, NULL, 0);
    for (s = 1; s < w->nslots; s++) {
        unsigned short pre[2] = { (unsigned short)s, 0 };
        unsigned k;
        if (!w->live[s]) continue;
        buf_rec(b, RK_UNIT, pre, 4, w->units + (size_t)s * REC_UNIT_IMAGE, REC_UNIT_IMAGE);
        for (k = 0; k < 2; k++)
            if (w->lists[s * 2 + k].n)
                list_put(b, RK_ORDERS, s, k, w->lists[s * 2 + k].img, w->lists[s * 2 + k].n);
    }
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned short pre[2] = { (unsigned short)i, 0 };
        unsigned char v[4];
        buf_rec(b, RK_PLAYER, pre, 4, w->players[i], REC_PLAYER_DWORDS * 4);
        v[0] = (unsigned char)i;
        v[1] = w->ctl[i];
        v[2] = v[3] = 0;
        buf_rec(b, RK_CONTROLLER, v, 4, NULL, 0);
    }
    {
        unsigned char v[4];
        v[0] = w->view[0];
        v[1] = w->view[1];
        v[2] = v[3] = 0;
        buf_rec(b, RK_VIEWER, v, 4, NULL, 0);
    }
    for (i = 0; i < w->ntab; i++) {
        WTable* t = &w->tab[i];
        unsigned char pre[8];
        unsigned short ix = (unsigned short)t->index, z = 0;
        memcpy(pre, &ix, 2);
        memcpy(pre + 2, &z, 2);
        memcpy(pre + 4, &t->size, 4);
        buf_rec(b, RK_TABLE_SIZE, pre, 8, NULL, 0);
        for (off = 0; off < t->size; off += SPAN_CHUNK) {
            unsigned n = t->size - off < SPAN_CHUNK ? t->size - off : SPAN_CHUNK;
            if (all_zero(t->img + off, n)) continue;     /* RK_TABLE_SIZE has zeroed it */
            memcpy(pre + 4, &off, 4);
            buf_rec(b, RK_TABLE_SPAN, pre, 8, t->img + off, n);
        }
    }
    if (w->grid) grid_diff_put(b, w);
    for (i = 0; i < w->npool; i++) {
        static const unsigned char zero[REC_WRECK_BYTES];
        unsigned short pre[2] = { (unsigned short)i, 0 };
        const unsigned char* rec = w->pool + (size_t)i * REC_WRECK_BYTES;
        if (memcmp(rec, zero, REC_WRECK_BYTES)) buf_rec(b, RK_WRECK, pre, 4, rec, REC_WRECK_BYTES);
    }
    if (w->heads_in) buf_rec(b, RK_FEATURES, w->heads, sizeof w->heads, NULL, 0);
    if (w->globals_in) buf_rec(b, RK_GLOBALS, w->globals, sizeof w->globals, NULL, 0);
    if (w->phead_in) {
        unsigned head[2] = { w->pcount, w->phw };
        buf_rec(b, RK_PROJ_HEAD, head, sizeof head, NULL, 0);
        for (i = 0; i < w->phw; i++) {
            unsigned short pre[2] = { (unsigned short)i, 0 };
            buf_rec(b, RK_PROJ, pre, 4, w->proj + (size_t)i * REC_PROJ_IMAGE, REC_PROJ_IMAGE);
        }
    }
    if (w->mnext == w->mbytes) {
        unsigned off;
        for (off = 0; off < w->mbytes; off += REC_MAPPED_CHUNK) {
            unsigned n = w->mbytes - off < REC_MAPPED_CHUNK ? w->mbytes - off : REC_MAPPED_CHUNK;
            buf_rec(b, RK_MAPPED, &off, 4, w->mapped + off, n);
        }
    }
    if (b->oom) { fail(w, "no memory for a snapshot"); return; }
    block(w, RB_SNAPSHOT, w->tick, w->tick, b->p, b->n, 1);
    w->snap_tick = w->tick;
}

/* The iteration at `w->tick` is complete, the events of the step after it included: its stream
   block may end here, and a snapshot of the copy at `w->tick` may follow it. That snapshot comes
   after those events, so a reader that starts from it takes them from the block before. */
static void iteration_done(Writer* w)
{
    w->sticks++;
    if (w->tick - w->snap_tick >= SNAPSHOT_TICKS) {
        stream_flush(w);
        snapshot(w);
    } else if (w->stream.n >= STREAM_BYTES || w->sticks >= STREAM_TICKS) {
        stream_flush(w);
    }
}

/* ---- one record ----------------------------------------------------------------------------- */

/* An RK_ORDERS or RK_VERIFY_ORDERS: u16 slot, u8 list, u8 0, u16 the list's nodes, u16 the first
   one here, then whole node images. The first record of a list starts it, and the rest must be its
   own, in order, until every node has come. 0 when malformed. */
static int list_record(Writer* w, unsigned kind, const unsigned char* b, unsigned len)
{
    unsigned short slot, total, first;
    unsigned list, k;
    if (len < 8 || (len - 8) % REC_ORDER_IMAGE) return 0;
    memcpy(&slot, b, 2);
    memcpy(&total, b + 4, 2);
    memcpy(&first, b + 6, 2);
    list = b[2];
    k = (len - 8) / REC_ORDER_IMAGE;
    if (list > 1 || b[3] || !slot_ok(w, slot) || !w->live[slot] || total > REC_ORDERS_MAX ||
        k > REC_ORDERS_CHUNK || first + k > total || (total && !k))
        return 0;
    if (!first) {
        if (w->pend.next < w->pend.total) return 0;
        w->pend.kind = kind;
        w->pend.slot = slot;
        w->pend.list = list;
        w->pend.total = total;
        w->pend.next = 0;
        if (kind == RK_ORDERS && !list_resize(&w->lists[slot * 2 + list], total)) {
            fail(w, "no memory for an order list's copy");
            return 1;
        }
    } else if (w->pend.kind != kind || w->pend.slot != slot || w->pend.list != list ||
               w->pend.total != total || w->pend.next != first) {
        return 0;
    }
    w->pend.next = first + k;
    if (kind == RK_ORDERS) {
        memcpy(w->lists[slot * 2 + list].img + (size_t)first * REC_ORDER_IMAGE, b + 8,
               (size_t)k * REC_ORDER_IMAGE);
        buf_rec(&w->stream, kind, b, len, NULL, 0);
    } else {
        memcpy(w->vlist + (size_t)first * REC_ORDER_IMAGE, b + 8, (size_t)k * REC_ORDER_IMAGE);
        if (w->pend.next == total) verify_list(w, slot, list, w->vlist, total);
    }
    return 1;
}


/* The feature records and the brackets. 1 applied, 0 malformed. A bracket opens and closes inside
   one step's events: a feature call's, RK_EV_FEATURE to its END, holds no other bracket, and
   cells only when it is the tick's, whose cells are its input and none of the copy, and which is
   inside no bracket; a death's, RK_EV_DEATH to its END, holds the feature calls its explosion
   made and may hold deaths. A draw is inside one. */
static int feature_record(Writer* w, unsigned kind, const unsigned char* b, unsigned len)
{
    unsigned short h[4];
    unsigned id;
    int open = w->fbracket || w->ndeaths;
    switch (kind) {
    case RK_GRID:
        if (len != 4 || w->grid || !w->ticked || open) return 0;
        memcpy(h, b, 4);
        if (h[0] != w->gw || h[1] != w->gh || !w->ring->grid) return 0;
        w->grid = w->ring->grid;
        w->base = (unsigned char*)malloc((size_t)w->gw * w->gh * REC_CELL_BYTES);
        if (!w->base) { fail(w, "no memory for the snapshots' base grid"); return 1; }
        memcpy(w->base, w->grid, (size_t)w->gw * w->gh * REC_CELL_BYTES);
        grid_put(&w->stream, w);
        return 1;
    case RK_CELLS:
        if (len < 8 || !w->grid) return 0;
        memcpy(h, b, 8);
        if (!h[2] || h[2] > REC_CELLS_MAX || h[3] || len != 8 + h[2] * REC_CELL_BYTES ||
            h[0] + h[2] > w->gw || h[1] >= w->gh)
            return 0;
        if (open && w->fbracket != FR_TICK + 1) return 0;
        if (!open)
            memcpy(w->grid + ((size_t)h[1] * w->gw + h[0]) * REC_CELL_BYTES, b + 8,
                   (size_t)h[2] * REC_CELL_BYTES);
        break;
    case RK_WRECK:
        if (len != 4 + REC_WRECK_BYTES || open) return 0;
        memcpy(h, b, 4);
        if (h[0] >= w->npool || h[1]) return 0;
        memcpy(w->pool + (size_t)h[0] * REC_WRECK_BYTES, b + 4, REC_WRECK_BYTES);
        break;
    case RK_FEATURES:
        if (len != 16 || open) return 0;
        memcpy(w->heads, b, 16);
        w->heads_in = 1;
        break;
    case RK_EV_FEATURE:
        if (!w->ticked || w->fbracket || (b[0] == FR_TICK && w->ndeaths) ||
            b[1] > REC_PHASE_LATE || b[2] || b[3] ||
            !rec_root_len(b[0], len))
            return 0;
        w->fbracket = b[0] + 1u;
        break;
    case RK_EV_FEATURE_END:
        if (len != 8 || w->fbracket != b[0] + 1u || b[1] || b[2] || b[3]) return 0;
        w->fbracket = 0;
        break;
    case RK_EV_DEATH:
        if (!w->ticked || w->fbracket || w->ndeaths == REC_DEATHS_MAX || !rec_event_len(kind, len))
            return 0;
        memcpy(&w->death[w->ndeaths++], b, 4);
        break;
    case RK_EV_DEATH_END:
        if (len != 4 || !w->ndeaths) return 0;
        memcpy(&id, b, 4);
        if (id != w->death[w->ndeaths - 1]) return 0;
        w->ndeaths--;
        break;
    case RK_EV_DRAW:
        if (!open || len != 12 || b[0] >= FS_SITES || b[1] || b[2] || b[3]) return 0;
        break;
    default:
        return 0;
    }
    buf_rec(&w->stream, kind, b, len, NULL, 0);
    return 1;
}

/* An RK_MAPPED_RUNS's runs, checked whole before `apply` writes any into `map`: 1 well formed. */
static int mapped_runs(unsigned char* map, unsigned bytes, const unsigned char* b, unsigned len,
                       int apply)
{
    unsigned at = 0, v[2];
    if (!len || len > REC_MAPPED_RUNS) return 0;
    if (apply && !mapped_runs(map, bytes, b, len, 0)) return 0;
    while (at < len) {
        if (len - at < 8) return 0;
        memcpy(v, b + at, 8);
        if (!v[1] || v[1] > len - at - 8 || v[0] > bytes || v[1] > bytes - v[0]) return 0;
        if (apply) memcpy(map + v[0], b + at + 8, v[1]);
        at += (8 + v[1] + 3) & ~3u;
    }
    return 1;
}

/* The globals, the pool's records and the explored map, all taken at a capture point: outside
   every bracket. 1 applied, 0 malformed. */
static int globals_proj_record(Writer* w, unsigned kind, const unsigned char* b, unsigned len)
{
    unsigned short ix = 0;
    unsigned n = 0, v[3];
    if (w->fbracket || w->ndeaths) return 0;
    if (len >= 2) memcpy(&ix, b, 2);
    switch (kind) {
    case RK_GLOBALS:
        if (len != sizeof w->globals) return 0;
        memcpy(w->globals, b, len);
        w->globals_in = 1;
        break;
    case RK_GLOBALS_DELTA:
        if (len < REC_GLOBALS_DELTA_HEAD || !w->globals_in ||
            !delta_count(b, REC_GLOBALS_BITS, REC_GLOBALS_DWORDS, &n) ||
            len != REC_GLOBALS_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply(w->globals, b, REC_GLOBALS_DWORDS, b + REC_GLOBALS_DELTA_HEAD);
        break;
    case RK_PROJ_HEAD:
        if (len != 8) return 0;
        memcpy(v, b, 8);
        if (v[0] > v[1] || v[1] > w->ncap) return 0;
        proj_head(w, v[0], v[1]);
        break;
    case RK_PROJ:
        if (len != 4 + REC_PROJ_IMAGE || b[2] || b[3] || !w->phead_in || ix >= w->phw) return 0;
        memcpy(w->proj + (size_t)ix * REC_PROJ_IMAGE, b + 4, REC_PROJ_IMAGE);
        break;
    case RK_PROJ_DELTA:
        if (len < REC_PROJ_DELTA_HEAD || !w->phead_in || ix >= w->phw ||
            !delta_count(b + 2, REC_PROJ_BITS, REC_PROJ_DWORDS, &n) ||
            len != REC_PROJ_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply((unsigned*)(w->proj + (size_t)ix * REC_PROJ_IMAGE), b + 2, REC_PROJ_DWORDS,
                    b + REC_PROJ_DELTA_HEAD);
        break;
    case RK_MAPPED:
        /* the map whole, in order from offset 0, once (cap_mapped) */
        if (len < 5 || len > 4 + REC_MAPPED_CHUNK) return 0;
        memcpy(v, b, 4);
        if (v[0] != w->mnext || len - 4 > w->mbytes - v[0]) return 0;
        memcpy(w->mapped + v[0], b + 4, len - 4);
        w->mnext += len - 4;
        break;
    case RK_MAPPED_RUNS:
        if (w->mnext != w->mbytes || !mapped_runs(w->mapped, w->mbytes, b, len, 1)) return 0;
        break;
    case RK_VERIFY_GLOBALS:
        if (len != sizeof w->globals || !w->globals_in) return 0;
        verify_globals(w, b);
        return 1;
    case RK_VERIFY_PROJ:
        if (len < 12 || (len - 12) % REC_PROJ_IMAGE) return 0;
        memcpy(v, b, 12);
        n = (len - 12) / REC_PROJ_IMAGE;
        if (v[0] > v[1] || v[1] > w->ncap || v[2] > v[1] || n > v[1] - v[2]) return 0;
        verify_proj(w, v[0], v[1], v[2], b + 12, n);
        return 1;
    default:
        return 0;
    }
    buf_rec(&w->stream, kind, b, len, NULL, 0);
    return 1;
}

/* 1 applied, 0 malformed */
static int apply(Writer* w, unsigned kind, const unsigned char* b, unsigned len)
{
    unsigned short id = 0;
    unsigned n = 0, off = 0;
    WTable* t;
    if (!w->header) {
        if (kind != RK_HEADER) return 0;
        w->header = 1;
        return header_apply(w, b, len);
    }
    /* a list that has begun is followed by the rest of it and nothing else */
    if (w->pend.next < w->pend.total && kind != w->pend.kind) return 0;
    if (len >= 2) memcpy(&id, b, 2);
    switch (kind) {
    case RK_TICK:
        if (len != 4 || w->fbracket || w->ndeaths) return 0;
        if (w->ticked) iteration_done(w);
        if (w->dead) return 1;
        memcpy(&w->tick, b, 4);
        if (!w->ticked) w->snap_tick = w->tick;
        if (!w->stream.n) w->sfirst = w->tick;
        w->ticked = 1;
        w->iterations++;
        buf_rec(&w->stream, RK_TICK, b, 4, NULL, 0);
        return 1;
    case RK_UNIT:
        if (len != 4 + REC_UNIT_IMAGE || !slot_ok(w, id)) return 0;
        memcpy(w->units + (size_t)id * REC_UNIT_IMAGE, b + 4, REC_UNIT_IMAGE);
        w->live[id] = 1;
        break;
    case RK_UNIT_DELTA:
        if (len < REC_UNIT_DELTA_HEAD || !slot_ok(w, id) || !w->live[id] ||
            !delta_count(b + 2, REC_UNIT_BITS, REC_UNIT_DWORDS, &n) ||
            len != REC_UNIT_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply((unsigned*)(w->units + (size_t)id * REC_UNIT_IMAGE), b + 2, REC_UNIT_DWORDS,
                    b + REC_UNIT_DELTA_HEAD);
        break;
    case RK_UNIT_GONE:
        if (len != 4 || !slot_ok(w, id)) return 0;
        w->live[id] = 0;
        list_clear(w, id);
        break;
    case RK_PLAYER:
        if (len != 4 + REC_PLAYER_DWORDS * 4 || id >= REC_PLAYERS) return 0;
        memcpy(w->players[id], b + 4, REC_PLAYER_DWORDS * 4);
        break;
    case RK_PLAYER_DELTA:
        if (len < REC_PLAYER_DELTA_HEAD || id >= REC_PLAYERS ||
            !delta_count(b + 2, REC_PLAYER_BITS, REC_PLAYER_DWORDS, &n) ||
            len != REC_PLAYER_DELTA_HEAD + 4 * n)
            return 0;
        delta_apply(w->players[id], b + 2, REC_PLAYER_DWORDS, b + REC_PLAYER_DELTA_HEAD);
        break;
    case RK_TABLE_SIZE:
        if (len != 8 || !(t = table_of(w, id))) return 0;
        memcpy(&n, b + 4, 4);
        if (n % t->stride) return 0;
        if (!table_resize(t, n)) { fail(w, "no memory for a table's copy"); return 1; }
        break;
    case RK_TABLE_SPAN:
        if (len < 8 || !(t = table_of(w, id))) return 0;
        memcpy(&off, b + 4, 4);
        n = len - 8;
        if (off > t->size || n > t->size - off) return 0;
        memcpy(t->img + off, b + 8, n);
        break;
    case RK_ORDERS:
    case RK_VERIFY_ORDERS:
        return list_record(w, kind, b, len);
    case RK_CONTROLLER:
        if (len != 4 || b[0] >= REC_PLAYERS || b[2] || b[3]) return 0;
        w->ctl[b[0]] = b[1];
        break;
    case RK_VIEWER:
        if (len != 4 || b[2] || b[3]) return 0;
        w->view[0] = b[0];
        w->view[1] = b[1];
        break;
    case RK_VERIFY_UNIT: {
        unsigned short live;
        if (len < 4 || !slot_ok(w, id)) return 0;
        memcpy(&live, b + 2, 2);
        if (live > 1 || len != (live ? 4 + REC_UNIT_IMAGE : 4)) return 0;
        verify_unit(w, id, live, b + 4);
        return 1;
    }
    case RK_VERIFY_PLAYER:
        if (len != 4 + REC_PLAYER_DWORDS * 4 || id >= REC_PLAYERS) return 0;
        verify_player(w, id, b + 4);
        return 1;
    case RK_VERIFY_TABLE:
        if (len < 8 || !(t = table_of(w, id))) return 0;
        memcpy(&off, b + 4, 4);
        n = len - 8;
        if (off > t->size || n > t->size - off) return 0;
        verify_table(w, t, off, b + 8, n);
        return 1;
    case RK_END:
        if (len != 24 || w->fbracket || w->ndeaths) return 0;
        memcpy(&id, b, 2);
        w->end_reason = id;
        memcpy(&w->bad_ids, b + 4, 4);
        memcpy(&w->ring_high, b + 8, 4);
        memcpy(&w->off_thread, b + 12, 4);
        memcpy(&w->uncredited, b + 16, 4);
        memcpy(&w->unobserved, b + 20, 4);
        if (w->ticked) iteration_done(w);
        return 1;
    case RK_GRID:
    case RK_CELLS:
    case RK_WRECK:
    case RK_FEATURES:
    case RK_EV_FEATURE:
    case RK_EV_FEATURE_END:
    case RK_EV_DRAW:
    case RK_EV_DEATH:
    case RK_EV_DEATH_END:
        return feature_record(w, kind, b, len);
    case RK_GLOBALS:
    case RK_GLOBALS_DELTA:
    case RK_PROJ_HEAD:
    case RK_PROJ:
    case RK_PROJ_DELTA:
    case RK_VERIFY_GLOBALS:
    case RK_VERIFY_PROJ:
    case RK_MAPPED:
    case RK_MAPPED_RUNS:
        return globals_proj_record(w, kind, b, len);
    default:
        /* the events: nothing of the copy; RK_CORRECT and RK_HEADER are never in the ring after
           the first record */
        if (!w->ticked || !rec_event_len(kind, len)) return 0;
        break;
    }
    buf_rec(&w->stream, kind, b, len, NULL, 0);
    return 1;
}

/* ---- the end -------------------------------------------------------------------------------- */

static void tlv(Buf* b, unsigned tag, const void* p, unsigned n)
{
    static const unsigned char zero[4];
    unsigned short h[2];
    h[0] = (unsigned short)tag;
    h[1] = (unsigned short)n;
    buf_put(b, h, 4);
    buf_put(b, p, n);
    buf_put(b, zero, ((n + 3) & ~3u) - n);
}

static void tlv_u32(Buf* b, unsigned tag, unsigned v) { tlv(b, tag, &v, 4); }

static void finish(Writer* w)
{
    Buf e = { 0 };
    unsigned i;
    if (!w->dead && w->header) {
        stream_flush(w);
        tlv_u32(&e, RT_END_REASON, w->end_reason);
        tlv_u32(&e, RT_LAST, w->tick);
        tlv_u32(&e, RT_ITERATIONS, w->iterations);
        for (i = 0; i < w->ncorr; i++) {
            unsigned char c[12];
            unsigned short tb = (unsigned short)w->corr[i].table;
            c[0] = (unsigned char)w->corr[i].kind;
            c[1] = 0;
            memcpy(c + 2, &tb, 2);
            memcpy(c + 4, &w->corr[i].off, 4);
            memcpy(c + 8, &w->corr[i].count, 4);
            tlv(&e, RT_CORRECTIONS, c, 12);
        }
        if (w->corr_more) {
            unsigned char c[12] = { 0 };
            unsigned all = 0xFFFFFFFFu;
            memcpy(c + 4, &all, 4);
            memcpy(c + 8, &w->corr_more, 4);
            tlv(&e, RT_CORRECTIONS, c, 12);
        }
        tlv_u32(&e, RT_BAD_IDS, w->bad_ids);
        tlv_u32(&e, RT_RING_HIGH, w->ring_high);
        tlv_u32(&e, RT_OFF_THREAD, w->off_thread);
        tlv_u32(&e, RT_UNCREDITED, w->uncredited);
        tlv_u32(&e, RT_UNOBSERVED, w->unobserved);
        if (e.oom) fail(w, "no memory for the end block");
        else block(w, RB_END, w->tick, w->tick, e.p, e.n, 0);
    }
    if (!w->dead && w->file != INVALID_HANDLE_VALUE) {
        unsigned corr = w->corr_more;
        for (i = 0; i < w->ncorr; i++) corr += w->corr[i].count;
        CloseHandle(w->file);
        tagpu_logf("recorder: writer: %s closed: %u iterations to GameTime %u, %u corrections, "
                   "stream %u KB deflated to %u KB, the ring held %u KB at most", w->path,
                   w->iterations, w->tick, corr, (unsigned)(w->raw >> 10),
                   (unsigned)(w->packed >> 10), w->ring_high >> 10);
    }
    free(e.p);
    free(w->stream.p);
    free(w->snap.p);
    for (i = 0; i < w->ntab; i++) free(w->tab[i].img);
    if (w->units) VirtualFree(w->units, 0, MEM_RELEASE);
    for (i = 0; w->lists && i < w->nslots * 2; i++) free(w->lists[i].img);
    free(w->lists);
    free(w->vlist);
    free(w->live);
    free(w->pool);
    free(w->proj);
    free(w->mapped);
    free(w->base);
}

/* ---- the thread ----------------------------------------------------------------------------- */

static DWORD WINAPI writer_main(void* arg)
{
    RecRing* g = (RecRing*)arg;
    Writer* w = (Writer*)calloc(1, sizeof *w);
    HANDLE done = g->done;
    unsigned r = 0;
    int stop = 0;
    if (w) {
        w->ring = g;
        w->file = INVALID_HANDLE_VALUE;
        snprintf(w->path, sizeof w->path, "%s", g->path);
    } else {
        InterlockedExchange(&g->failed, 1);
        tagpu_logf("recorder: writer: no memory: %s is not written", g->path);
    }
    while (!stop) {
        unsigned wp;
        WaitForSingleObject(g->wake, REC_WRITER_POLL_MS);   /* tagpu_recorder.h, *Waking* */
        wp = (unsigned)InterlockedCompareExchange(&g->wpub, 0, 0);
        while (w && !w->dead && r != wp) {
            unsigned pos = r & (REC_RING_BYTES - 1), avail = wp - r, size;
            RecHdr h;
            memcpy(&h, g->buf + pos, 4);
            if (h.kind == RK_PAD) {
                if (REC_RING_BYTES - pos > avail) { fail(w, "a malformed pad in the ring"); break; }
                r += REC_RING_BYTES - pos;
                continue;
            }
            size = (4 + h.len + 3) & ~3u;
            if (h.kind >= RK_KINDS || size > avail || pos + size > REC_RING_BYTES ||
                !apply(w, h.kind, g->buf + pos + 4, h.len)) {
                char why[80];
                snprintf(why, sizeof why, "a malformed record (kind %u, %u bytes) at ring byte %u",
                         h.kind, h.len, r);
                fail(w, why);
                break;
            }
            r += size;
        }
        if (!w || w->dead) r = wp;        /* parse nothing more: only the hand-over matters */
        InterlockedExchange(&g->rpub, (LONG)r);
        stop = InterlockedCompareExchange(&g->ended, 0, 0) &&
               wp == (unsigned)InterlockedCompareExchange(&g->wend, 0, 0);
    }
    /* the hand-over (tagpu_recorder.h): the capture's last access to the ring is behind us */
    if (w) {
        finish(w);
        free(w);
    }
    VirtualFree(g->buf, 0, MEM_RELEASE);
    if (g->grid) VirtualFree(g->grid, 0, MEM_RELEASE);     /* the copy's grid, when it took it */
    CloseHandle(g->wake);
    HeapFree(GetProcessHeap(), 0, g);
    SetEvent(done);        /* the file is closed: the exit may go on (tagpu_recorder.h, *The exit*) */
    CloseHandle(done);
    return 0;
}

int tagpu_recwriter_start(RecRing* ring)
{
    HANDLE t = CreateThread(NULL, 0, writer_main, ring, 0, NULL);
    if (!t) return 0;
    CloseHandle(t);
    return 1;
}
