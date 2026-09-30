/* tagpu_recwriter.c -- the demo recorder's writer thread
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S1; the records are tagpu_recfmt.h's).

   It reads no engine memory. Everything it knows arrives through the ring (tagpu_recorder.h), and
   this file is not on thread-split.allow, so the build refuses an engine address here. It keeps a
   copy of the recorded world built from the records alone, compares it with the fresh images the
   capture sends each iteration, and writes a difference into the file as a correction, counted and
   logged. The file is blocks (RecBlock): the header, stream blocks of whole iterations, a snapshot
   of the copy every SNAPSHOT_TICKS, and the end.

   A record is checked against the copy's bounds before it is applied: a slot, a player, a table
   index, an offset and a length. One that fails is a fault in the capture, not in the game: it
   abandons the file (the ring's `failed`, which ends the recording at the next capture) and is
   logged. The writer then only waits for the hand-over, and parses nothing more. */
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
#define UNIT_LIVE_OFF  REC_UNIT_BYTES /* a correction counter's offset for a unit's liveness        */

typedef struct Buf { unsigned char* p; size_t n, cap; int oom; } Buf;

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
    unsigned       players[REC_PLAYERS][REC_PLAYER_DWORDS];
    WTable         tab[REC_TABLE_MAX];
    unsigned       ntab;

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

    unsigned       end_reason, bad_ids, ring_high;
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
        if (tag == RT_TABLE && n >= 12) {
            WTable* t;
            unsigned short ix;
            unsigned stride, nn = n - 12;
            if (w->ntab >= REC_TABLE_MAX) return 0;
            memcpy(&ix, b + at + 4, 2);
            memcpy(&stride, b + at + 8, 4);
            if (!stride || stride > SPAN_CHUNK || table_of(w, ix)) return 0;
            t = &w->tab[w->ntab++];
            t->index = ix;
            t->stride = stride;
            if (nn >= sizeof t->name) nn = sizeof t->name - 1;
            memcpy(t->name, b + at + 16, nn);
            t->name[nn] = 0;
        }
        at += (4 + n + 3) & ~3u;
    }
    if (w->nslots < 2 || w->nslots > 0xFFFF) return 0;
    w->units = (unsigned char*)VirtualAlloc(NULL, (size_t)w->nslots * REC_UNIT_BYTES,
                                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    w->live = (unsigned char*)calloc(w->nslots, 1);
    if (!w->units || !w->live) { fail(w, "no memory for the copy"); return 1; }
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

static void verify_unit(Writer* w, unsigned slot, unsigned live, const unsigned char* img)
{
    unsigned char* have = w->units + (size_t)slot * REC_UNIT_BYTES;
    unsigned short pre[2] = { (unsigned short)slot, 0 };
    unsigned corr0 = 0, i;
    if (live != w->live[slot]) {
        corr_count(w, RK_UNIT, 0, UNIT_LIVE_OFF);
        corr_log(w, "unit", slot, UNIT_LIVE_OFF, w->live[slot], live);
    } else if (live && memcmp(have, img, REC_UNIT_BYTES)) {
        for (i = 0; i < REC_UNIT_BYTES; i += 4) {
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
        memcpy(have, img, REC_UNIT_BYTES);
        buf_rec(&w->stream, RK_UNIT, pre, 4, img, REC_UNIT_BYTES);
    } else {
        buf_rec(&w->stream, RK_UNIT_GONE, pre, 4, NULL, 0);
    }
    w->live[slot] = (unsigned char)live;
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

/* ---- blocks of the stream ------------------------------------------------------------------- */

static void stream_flush(Writer* w)
{
    if (!w->stream.n) return;
    if (w->stream.oom) { fail(w, "no memory for a stream block"); return; }
    block(w, RB_STREAM, w->sfirst, w->tick, w->stream.p, w->stream.n, 1);
    w->stream.n = 0;
    w->sticks = 0;
}

/* The copy as records that build it from nothing, at the iteration just completed. */
static void snapshot(Writer* w)
{
    Buf* b = &w->snap;
    unsigned s, i, off;
    b->n = 0;
    buf_rec(b, RK_TICK, &w->tick, 4, NULL, 0);
    for (s = 1; s < w->nslots; s++) {
        unsigned short pre[2] = { (unsigned short)s, 0 };
        if (w->live[s])
            buf_rec(b, RK_UNIT, pre, 4, w->units + (size_t)s * REC_UNIT_BYTES, REC_UNIT_BYTES);
    }
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned short pre[2] = { (unsigned short)i, 0 };
        buf_rec(b, RK_PLAYER, pre, 4, w->players[i], REC_PLAYER_DWORDS * 4);
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
            memcpy(pre + 4, &off, 4);
            buf_rec(b, RK_TABLE_SPAN, pre, 8, t->img + off, n);
        }
    }
    if (b->oom) { fail(w, "no memory for a snapshot"); return; }
    block(w, RB_SNAPSHOT, w->tick, w->tick, b->p, b->n, 1);
    w->snap_tick = w->tick;
}

/* The iteration at `w->tick` is complete: its stream block may end here, and a snapshot may
   follow it. */
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
    if (len >= 2) memcpy(&id, b, 2);
    switch (kind) {
    case RK_TICK:
        if (len != 4) return 0;
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
        if (len != 4 + REC_UNIT_BYTES || !slot_ok(w, id)) return 0;
        memcpy(w->units + (size_t)id * REC_UNIT_BYTES, b + 4, REC_UNIT_BYTES);
        w->live[id] = 1;
        break;
    case RK_UNIT_DELTA:
        if (len < 12 || !slot_ok(w, id) || !w->live[id] ||
            !delta_count(b + 2, REC_UNIT_BITS, REC_UNIT_DWORDS, &n) || len != 12 + 4 * n)
            return 0;
        delta_apply((unsigned*)(w->units + (size_t)id * REC_UNIT_BYTES), b + 2, REC_UNIT_DWORDS,
                    b + 12);
        break;
    case RK_UNIT_GONE:
        if (len != 4 || !slot_ok(w, id)) return 0;
        w->live[id] = 0;
        break;
    case RK_PLAYER:
        if (len != 4 + REC_PLAYER_DWORDS * 4 || id >= REC_PLAYERS) return 0;
        memcpy(w->players[id], b + 4, REC_PLAYER_DWORDS * 4);
        break;
    case RK_PLAYER_DELTA:
        if (len < 16 || id >= REC_PLAYERS ||
            !delta_count(b + 2, REC_PLAYER_BITS, REC_PLAYER_DWORDS, &n) || len != 16 + 4 * n)
            return 0;
        delta_apply(w->players[id], b + 2, REC_PLAYER_DWORDS, b + 16);
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
    case RK_VERIFY_UNIT: {
        unsigned short live;
        if (len < 4 || !slot_ok(w, id)) return 0;
        memcpy(&live, b + 2, 2);
        if (live > 1 || len != (live ? 4 + REC_UNIT_BYTES : 4)) return 0;
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
        if (len != 12) return 0;
        memcpy(&id, b, 2);
        w->end_reason = id;
        memcpy(&w->bad_ids, b + 4, 4);
        memcpy(&w->ring_high, b + 8, 4);
        if (w->ticked) iteration_done(w);
        return 1;
    default:                         /* RK_CORRECT and RK_HEADER are never in the ring after it */
        return 0;
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
    free(w->live);
}

/* ---- the thread ----------------------------------------------------------------------------- */

static DWORD WINAPI writer_main(void* arg)
{
    RecRing* g = (RecRing*)arg;
    Writer* w = (Writer*)calloc(1, sizeof *w);
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
    CloseHandle(g->wake);
    HeapFree(GetProcessHeap(), 0, g);
    return 0;
}

int tagpu_recwriter_start(RecRing* ring)
{
    HANDLE t = CreateThread(NULL, 0, writer_main, ring, 0, NULL);
    if (!t) return 0;
    CloseHandle(t);
    return 1;
}
