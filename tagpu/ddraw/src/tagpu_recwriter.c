/* tagpu_recwriter.c -- the demo recorder's writer thread
   (research/notes/tadr-port/demo-recorder-skirmish-slice.md, S1 and S2a; the records are
   tagpu_recfmt.h's).

   It reads no engine memory. Everything it knows arrives through the ring (tagpu_recorder.h), and
   this file is not on thread-split.allow, so the build refuses an engine address here. It keeps a
   copy of the recorded world built from the records alone, compares it with the fresh images the
   capture sends each iteration, and writes a difference into the file as a correction, counted and
   logged. The file is blocks (RecBlock): the header, stream blocks of whole iterations, a snapshot
   of the copy every SNAPSHOT_TICKS, and the end. A stream block goes through the stream's codec
   (tagpu_reccodec.c), which writes it as a sectioned container, a chunk of its run's zstd frame,
   when that decodes back to its records, and as the records in a zstd frame of their own
   otherwise (stream_block); a snapshot is packed against the one before it (snapshot).

   THE STREAM IS LOSSY. Each completed iteration's records, exact as the capture made them and the
   corrections added, go through the lossy filter (tagpu_reclossy.c) into the stream: a skirmish
   recording's stream holds the bytes RT_HOLD names between refreshes, and the copy stays exact.
   At a refresh the stream's world is checked equal to the copy, and a snapshot follows only a
   refresh (iteration_done), so every snapshot is a world the stream itself reaches. A multiplayer
   recording's stream holds nothing: its iterations are written as they came.

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

   A SNAPSHOT is smaller than the copy: its cells are the ones that differ from the grid at the
   end of the first iteration, the base, and its wreck records the ones that differ from the pool
   then, the wreck base; the writer takes both there (iteration_done), and a reader at the end of
   the first iteration it reads; and a table's spans that are all zero are left out, since a
   snapshot builds from nothing and RK_TABLE_SIZE zeroes the table.

   A FORK (tagpu_recorder.h, RecFork) is written as a segment of the file it was played from, at
   the append point the takeover found, through the handle it opened (file_append). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_recorder.h"
#include "tagpu_recmp_history.h"
#include "tagpu_recfmt.h"
#include "tagpu_log.h"
#include "crc32.h"
#include "tagpu_reccodec.h"
#include "tagpu_reclossy.h"
#include "tagpu_rechold.h"
#include "zstd.h"

/* A stream block ends at an iteration past STREAM_BYTES or after STREAM_TICKS iterations, and
   before every snapshot. A block is what a reader takes at once and the writer checks at once;
   the frames' state and the run's zstd frame run on across blocks, so a block boundary costs the
   run only a flush; the codec declines a block of more than RECC_TICKS_MAX iterations. */
#define STREAM_BYTES   (2u << 20)
#define STREAM_TICKS   30
#define SNAPSHOT_TICKS 1800          /* 60 s of game time; why, and what it costs a seek:
                                        demo-recorder-size.md, L4                                   */
#define SPAN_CHUNK     0x7000        /* the most bytes one of a snapshot's spans carries            */
#define CORR_MAX       256           /* distinct (kind, table, offset) correction counters          */
#define CORR_LOG       16            /* corrections logged, a recording                             */
#define UNIT_LIVE_OFF  REC_UNIT_IMAGE /* a correction counter's offset for a unit's liveness        */

typedef RecBytes Buf;

/* an order list's images, in order: the filter reads the copy's lists as they are */
typedef RecLossyList WList;

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
    HANDLE         lock;
    HANDLE         file;
    int            append;      /* a fork: `file` is the ring's, written from its append point */
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

    Buf            iter;        /* the open iteration's records, exact               */
    Buf            stream;      /* the stream block being built, whole iterations   */
    unsigned       sfirst;      /* its first GameTime                               */
    unsigned       slast;       /* its last                                         */
    unsigned       sticks;      /* its iterations                                   */
    Buf            exact;       /* the test lever's RB_EXACT: the same iterations' records
                                   as `iter` held them                              */
    RecLossy*      lossy;       /* the filter; NULL for a multiplayer recording     */
    unsigned       refresh;     /* the refresh interval (RT_HOLD); REC_REFRESH_TICKS
                                   for a multiplayer recording, whose header has none */
    unsigned       refresh_tick; /* the last refresh's GameTime                     */
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
    int            evented;     /* an RCS_EVENTS record has come since the iteration's RK_TICK */

    unsigned char* base;        /* the grid at the end of the first iteration (a snapshot's
                                   cells differ), once iteration_done has taken it        */
    unsigned char* wbase;       /* the pool at the end of the first iteration (a snapshot's
                                   wreck records differ), once iteration_done has taken it */
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
    unsigned char  seats[4 + 12 * REC_PLAYERS];   /* RK_END's game end and RT_SEAT_ENDs   */
    unsigned long long raw, packed;   /* the stream's records, and the bytes written for them */
    unsigned       nstream;     /* the stream blocks written                             */
    unsigned       nsnap;       /* the snapshots written                                 */
    RecStream      codec;       /* the stream's codec (tagpu_reccodec.h), when codec_on    */
    int            codec_on;
    void*          cctx;        /* the ZSTD_CCtx when the codec is off; when it is on, the
                                   codec's packs every block (block_cctx)                 */
    RecBytes       pk;          /* the packed bytes of a block the codec does not write    */
    LONGLONG       codec_qpc, codec_max;   /* its time, in all and for the slowest block  */
    LONGLONG       snap_qpc, snap_max;     /* the snapshots' packing, the same            */
    RecMpFileState mp;
    RecMpHistory* mp_history;
    RecMpRecordEnd mp_end;
    Buf            contributions;
    unsigned       mp_in;

    unsigned       each;        /* RT_UNITS_EACH: a selection's most entries               */
    RecPov         pov[REC_PLAYERS][2];     /* each seat's last two samples, the earlier first */
    unsigned       pov_tick[REC_PLAYERS][2];   /* ... the GameTime each was taken          */
    unsigned char  npov[REC_PLAYERS];       /* ... how many of the two there are          */
    unsigned char  sel_in[REC_PLAYERS];     /* the seat has had an RK_SELECT               */
    unsigned short nsel[REC_PLAYERS];       /* its last one's entries                      */
    unsigned char  sel[REC_PLAYERS][REC_SEL_MAX * sizeof(RecSelEntry)];
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
    recfile_unlock(&w->lock);
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

/* A block's header and its body as given: `raw` the bytes the body unpacks to. */
static void block_put(Writer* w, unsigned kind, unsigned flags, unsigned first, unsigned last,
                      size_t raw, const unsigned char* body, size_t bn)
{
    RecBlock b;
    if (w->dead) return;
    memset(&b, 0, sizeof b);
    b.magic = REC_BLOCK_MAGIC;
    b.kind = (uint16_t)kind;
    b.flags = (uint16_t)flags;
    b.first = first;
    b.last = last;
    b.raw = (uint32_t)raw;
    b.packed = (uint32_t)bn;
    b.crc_body = (uint32_t)Crc32_ComputeBuf(0, body, bn);
    b.crc_head = (uint32_t)Crc32_ComputeBuf(0, &b, 28);
    put(w, &b, sizeof b);
    put(w, body, (DWORD)bn);
}

static void* block_cctx(Writer* w) { return w->codec_on ? w->codec.cctx : w->cctx; }

/* One block: a zstd frame at level 3 when that is smaller, else stored; `pack` 0 stores it (the
   header, the end and the timelines, which tagpu_recfmt.h has always stored). */
static void block(Writer* w, unsigned kind, unsigned first, unsigned last, const unsigned char* raw,
                  size_t n, int pack)
{
    void* cctx = block_cctx(w);
    int packed = pack && cctx && reccodec_pack(cctx, &w->pk, raw, n);
    block_put(w, kind, packed ? 0 : RBF_STORED, first, last, n, packed ? w->pk.p : raw,
              packed ? w->pk.n : n);
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
        {
            char why[128];
            w->lock = recfile_lock(w->path, why, sizeof why);
            if (!w->lock) {
                if (GetFileAttributesA(w->path) != INVALID_FILE_ATTRIBUTES) continue;
                fail(w, why);
                return 0;
            }
        }
        w->file = CreateFileA(w->path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
        if (w->file != INVALID_HANDLE_VALUE) {
            snprintf(w->ring->opened_path, sizeof w->ring->opened_path, "%s", w->path);
            return 1;
        }
        {
            DWORD error = GetLastError();
            recfile_unlock(&w->lock);
            if (error != ERROR_FILE_EXISTS || !dot) break;
        }
    }
    snprintf(w->path, sizeof w->path, "%s", w->ring->path);
    fail(w, "cannot create the file");
    return 0;
}

static void tlv(Buf* b, unsigned tag, const void* p, unsigned n);
static void tlv_u32(Buf* b, unsigned tag, unsigned v);

/* A fork's segment, from the append point on: the file cut there -- which drops a torn last block,
   or an unfinished segment with no complete iteration, its complete header block included, and no
   iteration of any timeline (tagpu_recreader.c, THE SCAN'S THREE OUTCOMES) -- and, when the
   segment before it has no end block, one that ends it at its last complete iteration. Then the
   header follows, as a new file's follows the magic.
   THE ORDERING that makes the append point current: the takeover found it after opening this
   handle (tagpu_replay.c, TK0), whose share mode (FILE_SHARE_READ) refuses every other open for
   writing from then on, and an earlier writer still holding the file would have made that open
   fail. So no byte past append_off has changed since the scan that found it. */
static int file_append(Writer* w)
{
    const RecFork* f = &w->ring->fork;
    LARGE_INTEGER at;
    snprintf(w->path, sizeof w->path, "%s", f->path);
    snprintf(w->ring->opened_path, sizeof w->ring->opened_path, "%s", w->path);
    at.QuadPart = (LONGLONG)f->append_off;
    if (!SetFilePointerEx(w->file, at, NULL, FILE_BEGIN) || !SetEndOfFile(w->file)) {
        char why[64];
        snprintf(why, sizeof why, "cannot append (%lu)", GetLastError());
        fail(w, why);
        return 0;
    }
    if (f->end_open) {
        Buf e = { 0 };
        tlv_u32(&e, RT_END_REASON, REC_END_UNFINISHED);
        tlv_u32(&e, RT_LAST, f->open_last);
        if (e.oom) {
            free(e.p);
            fail(w, "cannot append: no memory for the end block");
            return 0;
        }
        block(w, RB_END, f->open_last, f->open_last, e.p, e.n, 0);
        free(e.p);
        if (w->dead) return 0;
        tagpu_logf("recorder: writer: %s: timeline %u appended at byte %llu (timeline %u ended "
                   "unfinished at GameTime %u)", w->path, f->id, f->append_off, f->open_id, f->open_last);
    } else {
        tagpu_logf("recorder: writer: %s: timeline %u appended at byte %llu", w->path, f->id,
                   f->append_off);
    }
    return 1;
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

/* The codec from the header's RT_FEATURES, RT_CODEC, RT_PHYSICS and RT_AIR, parsed from the bytes a
   reader will parse, so the check decoder runs on the reader's constants. 1, or 0 when the reader
   would refuse them. Without RT_CODEC and RT_PHYSICS, with a grid or a wreck pool past the
   codec's bounds, or without memory for the encoder's and check decoder's states, every stream
   block is written as records. Without memory for the constants themselves
   the recording ends before its file is opened (fail): the entries cannot be checked without
   them, and a header a reader might refuse is never written. */
static int codec_begin(Writer* w, const unsigned char* const ent[3], const unsigned nent[3])
{
    RecCodecParams* p = (RecCodecParams*)malloc(sizeof *p);
    char why[160];
    if (!p) {
        fail(w, "no memory for the codec's constants");
        return 1;
    }
    if (!reccodec_params(p, w->nslots, w->ncap, w->gw, w->gh, w->npool, ent[0], nent[0], ent[1],
                         nent[1], ent[2], nent[2], why, sizeof why)) {
        tagpu_logf("recorder: writer: the header's codec entries are malformed: %s", why);
        free(p);
        return 0;
    }
    if (!p->usable)
        tagpu_logf("recorder: writer: %s: every stream block is written as records", why);
    else if (reccodec_stream_begin(&w->codec, p)) {
        w->codec_on = 1;
        w->codec.decline = w->ring->decline;
    } else
        tagpu_logf("recorder: writer: no memory for the codec of %u slots and %u projectiles: "
                   "every stream block is written as records", w->nslots, w->ncap);
    free(p);
    if (!w->codec_on) {
        w->cctx = ZSTD_createCCtx();
        if (!w->cctx)
            tagpu_logf("recorder: writer: no memory for zstd: every block is stored");
    }
    return 1;
}

/* `n` bytes from the 2n hex digits of `hex` */
static void hex_bytes(unsigned char* out, const char* hex, unsigned n)
{
    unsigned i;
    for (i = 0; i < 2 * n; i++) {
        char c = hex[i];
        unsigned v = c >= '0' && c <= '9' ? (unsigned)(c - '0') : (unsigned)((c | 0x20) - 'a' + 10);
        out[i / 2] = (unsigned char)(i & 1 ? out[i / 2] | v : v << 4);
    }
}

static int header_apply(Writer* w, const unsigned char* b, unsigned len)
{
    unsigned at = 0, mode = 0, each = 0, mapped = 0, seats = 0;
    const unsigned char* ent[3] = { NULL, NULL, NULL };     /* RT_CODEC, RT_PHYSICS, RT_AIR */
    unsigned nent[3] = { 0, 0, 0 };
    while (at + 4 <= len) {
        unsigned short tag, n;
        memcpy(&tag, b + at, 2);
        memcpy(&n, b + at + 2, 2);
        if (at + 4 + n > len) return 0;
        if (tag == RT_SLOTS && n == 4) memcpy(&w->nslots, b + at + 4, 4);
        if (tag == RT_MODE && n == 4) memcpy(&mode, b + at + 4, 4);
        if (tag == RT_UNITS_EACH && n == 4) memcpy(&each, b + at + 4, 4);
        if (tag == RT_MULTIPLAYER) {
            RecMpRecordHeader h;
            const RecMpRecordHeader* own = tagpu_recmp_file_header(w->ring->mp);
            if (w->mp_in || n != sizeof h || !own) return 0;
            memcpy(&h, b + at + 4, sizeof h);
            if (memcmp(&h, own, sizeof h) || !tagpu_recmp_file_state(&w->mp, &h)) return 0;
            w->mp_in = 1;
        }
        if (tag == RT_MP_SEATS) {
            const RecMpRoster* own = tagpu_recmp_file_roster(w->ring->mp);
            if (mapped || !own || n!=sizeof *own || memcmp(b+at+4,own,sizeof *own)) return 0;
            mapped=1;
        }
        if (tag == RT_PLAYER) {
            unsigned seat;
            if (n<16+REC_INFO_BYTES) return 0;
            seat=b[at+4];
            if (seat>=REC_PLAYERS || (seats>>seat&1)) return 0;
            seats|=1u<<seat;
            w->ctl[seat]=b[at+5];
        }
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
        if (tag >= RT_CODEC && tag <= RT_AIR) {
            if (ent[tag - RT_CODEC]) return 0;              /* each at most once */
            ent[tag - RT_CODEC] = b + at + 4;
            nent[tag - RT_CODEC] = n;
        }
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
    if (at!=len || (mode == 3) != !!w->mp_in || w->mp_in!=mapped || !!w->ring->mp != !!w->mp_in ||
        w->nslots < 2 || w->nslots > 0xFFFF || !w->gw || w->gw > 0xFFFF || !w->gh ||
        w->gh > 0xFFFF || !w->npool || w->npool > 0x8000 || !w->ncap || w->ncap > 0xFFFF)
        return 0;
    if (mapped) {
        const RecMpRoster* roster=tagpu_recmp_file_roster(w->ring->mp);
        unsigned i, mask=0;
        if (w->nslots!=roster->slots || each!=roster->each) return 0;
        for (i=0;i<roster->count;i++) {
            unsigned seat=roster->actor[i].seat, ctl=w->ctl[seat];
            mask|=1u<<seat;
            if (ctl<1 || ctl>4 || w->mp.header.actor[i].local!=(ctl==1 || ctl==2)) return 0;
        }
        if (seats!=mask) return 0;
        w->mp_history=tagpu_recmp_history_begin(&w->mp.header,roster,w->gw,w->gh);
        if (!w->mp_history) return 0;
    }
    if (!codec_begin(w, ent, nent)) return 0;
    if (w->dead) return 1;
    w->each = each;
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
    /* a reader of a file without RT_HOLD takes REC_REFRESH_TICKS (tarec.py, lossy-check), so the
       test lever's interval is a skirmish's alone */
    w->refresh = !w->mp_in && w->ring->refresh >= REC_REFRESH_MIN &&
                 w->ring->refresh <= REC_REFRESH_TICKS ? w->ring->refresh : REC_REFRESH_TICKS;
    /* the file says what its stream holds: a skirmish's RT_HOLD, after the capture's entries */
    if (!w->mp_in) {
        RecLossyParams lp;
        unsigned char hold[REC_HOLD_BYTES];
        unsigned flags = 0;
        _Static_assert(sizeof REC_HOLD_UNIT == REC_UNIT_IMAGE &&
                       sizeof REC_HOLD_NODE == REC_ORDER_IMAGE, "the hold masks' sizes");
        _Static_assert(sizeof TAGPU_REPLAY_HOLD_SHA256 == 65, "a SHA-256 in hex");
        lp.nslots = w->nslots;
        lp.hold_unit = REC_HOLD_UNIT;
        lp.hold_node = REC_HOLD_NODE;
        w->lossy = reclossy_begin(&lp);
        if (!w->lossy) {
            fail(w, "no memory for the lossy filter");
            return 1;
        }
        memcpy(hold, &w->refresh, 4);
        memcpy(hold + 4, &flags, 4);
        hex_bytes(hold + 8, TAGPU_REPLAY_HOLD_SHA256, 32);
        memcpy(hold + 40, REC_HOLD_UNIT, REC_UNIT_IMAGE);
        memcpy(hold + 40 + REC_UNIT_IMAGE, REC_HOLD_NODE, REC_ORDER_IMAGE);
        buf_put(&w->snap, b, len);
        tlv(&w->snap, RT_HOLD, hold, sizeof hold);
        if (w->snap.oom) {
            fail(w, "no memory for the header block");
            return 1;
        }
        b = w->snap.p;
        len = (unsigned)w->snap.n;
    }
    if (w->append) {
        if (!file_append(w)) return 1;
    } else {
        if (!file_open(w)) return 1;
        put(w, REC_MAGIC, 8);
    }
    block(w, RB_HEADER, 0, 0, b, len, 0);
    w->snap.n = 0;
    if (w->append)
        block(w, RB_TIMELINES, 0, 0, (const unsigned char*)w->ring->fork.states,
              w->ring->fork.nstates * sizeof(RecTimelineState), 0);
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
    buf_rec(&w->iter, RK_CORRECT, &corr0, 4, NULL, 0);
    if (live) {
        memcpy(have, img, REC_UNIT_IMAGE);
        buf_rec(&w->iter, RK_UNIT, pre, 4, img, REC_UNIT_IMAGE);
    } else {
        buf_rec(&w->iter, RK_UNIT_GONE, pre, 4, NULL, 0);
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
    buf_rec(&w->iter, RK_CORRECT, &corr0, 4, NULL, 0);
    list_put(&w->iter, RK_ORDERS, slot, list, img, n);
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
    buf_rec(&w->iter, RK_CORRECT, &corr0, 4, NULL, 0);
    buf_rec(&w->iter, RK_PLAYER, pre, 4, img, REC_PLAYER_DWORDS * 4);
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
    buf_rec(&w->iter, RK_CORRECT, &corr0, 4, NULL, 0);
    buf_rec(&w->iter, RK_TABLE_SPAN, pre, 8, img, n);
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
    buf_rec(&w->iter, RK_CORRECT, &corr0, 4, NULL, 0);
    buf_rec(&w->iter, RK_GLOBALS, w->globals, sizeof w->globals, NULL, 0);
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
        buf_rec(&w->iter, RK_CORRECT, &corr0, 4, NULL, 0);
        buf_rec(&w->iter, RK_PROJ_HEAD, head, sizeof head, NULL, 0);
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
        buf_rec(&w->iter, RK_CORRECT, &corr0, 4, NULL, 0);
        buf_rec(&w->iter, RK_PROJ, pre, 4, live, REC_PROJ_IMAGE);
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

/* A stream block of the records [p, p + n), whole iterations from GameTime `first` to `last`:
   through the codec a sectioned block when it decodes back, else the records
   (reccodec_stream_block, tagpu_reccodec.c's THREE INVARIANTS).
   DECODE BEFORE WRITE: the block is sectioned only when the check decoder D has given these very
   records back from the very bytes written; otherwise the records are written, so a codec bug
   costs size, never data. D is a second state and not the encoder's: it starts where a reader
   starts, from the units alone, and a check that reused the encoder's state would share its
   arithmetic and so its faults.
   RESET POINTS: after a records block, and after every snapshot (snapshot), the next block
   restarts the frames (RBF_FRAME_RESET) from what such a reader holds there: the units, the pool
   and the grid (tagpu_reccodec.c, RESET POINTS). A reader that starts at that snapshot holds no
   frame state from before it, and one that read the records block holds none either. */
static void stream_block(Writer* w, unsigned first, unsigned last, const unsigned char* p, size_t n,
                         unsigned refresh)
{
    unsigned flags = RBF_STORED;
    const unsigned char* body = p;
    size_t bn = n, raw;
    if (w->dead) return;
    if (w->codec_on) {
        LARGE_INTEGER t0, t1;
        LONGLONG dt;
        int k;
        QueryPerformanceCounter(&t0);
        k = reccodec_stream_block(&w->codec, p, n, &flags, &body, &bn);
        QueryPerformanceCounter(&t1);
        dt = t1.QuadPart - t0.QuadPart;
        w->codec_qpc += dt;
        if (dt > w->codec_max) w->codec_max = dt;
        if (k == RECC_MISMATCH && w->codec.mismatched <= 4)
            tagpu_logf("recorder: writer: the codec did not decode back at GameTime %u-%u: "
                       "the block is written as records (a codec bug)", first, last);
        else if (k == RECC_DECLINED && w->codec.declined <= 4)
            tagpu_logf("recorder: writer: the codec declined GameTime %u-%u: the block is "
                       "written as records", first, last);
    } else if (w->cctx && reccodec_pack(w->cctx, &w->pk, p, n)) {
        flags = 0;
        body = w->pk.p;
        bn = w->pk.n;
    }
    raw = (flags & RBF_STORED) ? bn : (flags & RBF_SECTIONS) ? w->codec.sec.n : n;
    block_put(w, RB_STREAM, flags | refresh, first, last, raw, body, bn);
    w->raw += n;
    w->packed += bn;
    w->nstream++;
}

/* The stream block of the whole iterations [sfirst, slast], RBF_REFRESH when the last is a
   refresh, and under the test lever its exact records as an RB_EXACT of the same GameTimes. The
   open iteration stays in `iter`: a paused capture's flush (writer_main) writes only whole
   iterations, and its acknowledgement follows WriteFile, so a reader cannot index a half-written
   block. */
static void stream_flush(Writer* w, unsigned refresh)
{
    if (!w->stream.n) return;
    if (w->stream.oom || w->exact.oom) { fail(w, "no memory for a stream block"); return; }
    stream_block(w, w->sfirst, w->slast, w->stream.p, w->stream.n, refresh);
    if (w->ring->exact) block(w, RB_EXACT, w->sfirst, w->slast, w->exact.p, w->exact.n, 1);
    w->stream.n = 0;
    w->exact.n = 0;
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
    for (i = 0; i < REC_PLAYERS; i++) {
        unsigned k;
        for (k = 0; k < w->npov[i]; k++) {
            unsigned char at[REC_POV_AT_BYTES];
            memcpy(at, &w->pov_tick[i][k], 4);
            memcpy(at + 4, &w->pov[i][k], sizeof(RecPov));
            buf_rec(b, RK_POV_AT, at, sizeof at, NULL, 0);
        }
        if (w->sel_in[i]) {
            RecSel head;
            head.player = (uint8_t)i;
            head.flags = REC_SEL_BEGIN | REC_SEL_END;
            head.total = head.count = w->nsel[i];
            head.first = 0;
            buf_rec(b, RK_SELECT, &head, sizeof head, w->sel[i],
                    (unsigned)w->nsel[i] * sizeof(RecSelEntry));
        }
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
    /* the records that differ from the wreck base, a record now zero included: the reader starts
       the pool from its own base (tagpu_recreader.c, world_reset), not from zero */
    for (i = 0; i < w->npool; i++) {
        unsigned short pre[2] = { (unsigned short)i, 0 };
        const unsigned char* rec = w->pool + (size_t)i * REC_WRECK_BYTES;
        if (memcmp(rec, w->wbase + (size_t)i * REC_WRECK_BYTES, REC_WRECK_BYTES))
            buf_rec(b, RK_WRECK, pre, 4, rec, REC_WRECK_BYTES);
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
    /* The chain is the codec's (reccodec_snapshot's INVARIANTS: its prefix is this segment's
       last snapshot, a key at most every REC_CHAIN_MAX + 1); without the codec every snapshot is
       a key at level 3. Either way the next stream block restarts the frames and the run. */
    if (w->codec_on) {
        LARGE_INTEGER t0, t1;
        LONGLONG dt;
        unsigned flags;
        const unsigned char* body;
        size_t bn;
        QueryPerformanceCounter(&t0);
        reccodec_snapshot(&w->codec, b->p, b->n, &flags, &body, &bn);
        QueryPerformanceCounter(&t1);
        dt = t1.QuadPart - t0.QuadPart;
        w->snap_qpc += dt;
        if (dt > w->snap_max) w->snap_max = dt;
        block_put(w, RB_SNAPSHOT, flags, w->tick, w->tick, b->n, body, bn);
    } else {
        block(w, RB_SNAPSHOT, w->tick, w->tick, b->p, b->n, 1);
    }
    w->snap_tick = w->tick;
    w->nsnap++;
}

/* The iteration at `w->tick` is complete, the events of the step after it included: the filter
   writes it into the stream, its stream block may end here, and a snapshot of the copy at
   `w->tick` may follow it. That snapshot comes after those events, so a reader that starts from
   it takes them from the block before.
   THE REFRESH: the segment's first two iterations and then one in each interval, the GameTimes
   1 modulo it on consecutive iterations (reclossy_is_refresh), and every snapshot's -- a
   snapshot is taken only at a refresh, SNAPSHOT_TICKS or more after the last. The filter writes
   every difference there, so the stream's world equals the copy, and
   that is checked before the block ends: INVARIANT, an ordering, every snapshot follows a refresh
   at which the stream's world was found equal to the copy, so a reader that starts from a
   snapshot holds what a reader of the whole stream holds there. A failed check is a fault of the
   filter, and abandons the file rather than write a stream that does not reach its snapshots. The
   first iteration is a refresh because the bases below are the copy's, and a reader takes its
   own from the stream's first iteration: equal there, they are the same bytes. The second is
   one so that a takeover asked for inside the first interval has a refresh to start from after
   the segment's first iteration (reclossy_is_refresh).
   THE BASES are taken at the first call, before any snapshot can be: the wreck pool and the grid
   at the end of the segment's first iteration. INVARIANT: they equal every reader's
   (tagpu_recreader.c, tick_take), by construction -- one Writer is one segment, calloc'd per
   recording or fork, so its pool starts zero and holds exactly the RK_WRECKs of that segment's
   first iteration, and its grid exactly that iteration's images (feature_record refuses RK_GRID
   in any later iteration, and takes no cells that a reader takes as events), as the reader's do
   after world_reset(r, 0) and that iteration's tick_read; neither side applies a record of
   another segment, and both bound the index by the same pool and grid sizes (the headers' shape
   agrees across a chain). */
static void iteration_done(Writer* w)
{
    int refresh = reclossy_is_refresh(w->iterations, w->tick, w->refresh_tick, w->refresh);
    int snap = refresh && w->tick - w->snap_tick >= SNAPSHOT_TICKS;
    RecLossyExact x;
    if (!w->wbase) {
        w->wbase = (unsigned char*)malloc((size_t)w->npool * REC_WRECK_BYTES);
        if (!w->wbase) { fail(w, "no memory for the snapshots' wreck base"); return; }
        memcpy(w->wbase, w->pool, (size_t)w->npool * REC_WRECK_BYTES);
        if (w->grid) {
            w->base = (unsigned char*)malloc((size_t)w->gw * w->gh * REC_CELL_BYTES);
            if (!w->base) { fail(w, "no memory for the snapshots' base grid"); return; }
            memcpy(w->base, w->grid, (size_t)w->gw * w->gh * REC_CELL_BYTES);
        }
    }
    if (w->iter.oom) { fail(w, "no memory for an iteration"); return; }
    if (!w->stream.n) w->sfirst = w->tick;
    w->slast = w->tick;
    x.units = w->units;
    x.live = w->live;
    x.lists = w->lists;
    if (w->lossy ? !reclossy_iteration(w->lossy, w->iter.p, w->iter.n, refresh, &x, &w->stream)
                 : !buf_put(&w->stream, w->iter.p, w->iter.n)) {
        fail(w, w->stream.oom ? "no memory for a stream block" : "the lossy filter refused an "
             "iteration the writer took");
        return;
    }
    if (w->ring->exact) buf_put(&w->exact, w->iter.p, w->iter.n);
    w->iter.n = 0;
    w->sticks++;
    if (refresh) {
        if (w->lossy && !reclossy_equal(w->lossy, &x)) {
            fail(w, "the lossy world differs from the copy at a refresh");
            return;
        }
        w->refresh_tick = w->tick;
        stream_flush(w, RBF_REFRESH);
        if (snap) snapshot(w);
    } else if (w->stream.n >= STREAM_BYTES || w->sticks >= STREAM_TICKS) {
        stream_flush(w, 0);
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
        buf_rec(&w->iter, kind, b, len, NULL, 0);
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
   made, in a multiplayer recording its corpse's (FR_CORPSE, inside a death only), and may hold
   deaths. A draw is inside one, FS_SOURCE inside a multiplayer recording's tick only.
   INVARIANT, an ordering: the copy's grid takes exactly the cells every reader and the encoder
   take as images. Their rule (tagpu_recreader.c, apply; reccodec's events flag; codectest's
   world_apply) is that an RK_CELLS is an image when no RCS_EVENTS record came before it in its
   iteration, and tick input otherwise, which event_take accepts only inside the tick's feature
   call. So cells after an event are refused here unless that call is open, and the grid's first
   image (RK_GRID, written as cells where it stands) comes before every event of the segment's
   first iteration. A snapshot is this copy, and the cells frame restarts from the reader's grid
   at it: a cell the copy took and the readers did not would misdecode a seek, silently. */
static int feature_record(Writer* w, unsigned kind, const unsigned char* b, unsigned len)
{
    unsigned short h[4];
    unsigned id;
    int open = w->fbracket || w->ndeaths;
    switch (kind) {
    case RK_GRID:
        if (len != 4 || w->grid || w->iterations != 1 || open || w->evented) return 0;
        memcpy(h, b, 4);
        if (h[0] != w->gw || h[1] != w->gh || !w->ring->grid) return 0;
        w->grid = w->ring->grid;
        grid_put(&w->iter, w);
        return 1;
    case RK_CELLS:
        if (len < 8 || !w->grid) return 0;
        memcpy(h, b, 8);
        if (!h[2] || h[2] > REC_CELLS_MAX || h[3] || len != 8u + h[2] * REC_CELL_BYTES ||
            h[0] + h[2] > w->gw || h[1] >= w->gh)
            return 0;
        if (open ? w->fbracket != FR_TICK + 1 : w->evented) return 0;
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
            (b[0] == FR_CORPSE && (!w->ndeaths || !w->mp_in)) ||
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
        if (!open || len != 12 || b[0] >= FS_SITES || b[1] || b[2] || b[3] ||
            (b[0] == FS_SOURCE && (w->fbracket != FR_TICK + 1 || !w->mp_in)))
            return 0;
        break;
    default:
        return 0;
    }
    buf_rec(&w->iter, kind, b, len, NULL, 0);
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
    buf_rec(&w->iter, kind, b, len, NULL, 0);
    return 1;
}

/* THE PERSPECTIVE (tagpu_recpov.h): a skirmish's only, outside every bracket. A sample and a
   selection are taken with the images; a click between two capture points, so after the step's
   events. The copy keeps each seat's last two samples, dated by their iteration, and its last
   selection, whose slots lie inside the seat's block as the copy's player image names it -- the
   capture's own player images come first in an iteration. 1 applied, 0 malformed. */
static int pov_record(Writer* w, unsigned kind, const unsigned char* b, unsigned len)
{
    if (w->mp_in || !w->ticked || w->fbracket || w->ndeaths) return 0;
    if (kind == RK_POV) {
        RecPov p;
        unsigned s;
        if (len != sizeof p) return 0;
        memcpy(&p, b, sizeof p);
        s = p.player;
        if (s >= REC_PLAYERS || !rec_pov_ok(&p) ||
            (w->npov[s] && w->pov_tick[s][w->npov[s] - 1] >= w->tick))
            return 0;
        if (w->npov[s] == 2) {
            w->pov[s][0] = w->pov[s][1];
            w->pov_tick[s][0] = w->pov_tick[s][1];
            w->npov[s] = 1;
        }
        w->pov[s][w->npov[s]] = p;
        w->pov_tick[s][w->npov[s]] = w->tick;
        w->npov[s]++;
    } else if (kind == RK_CLICK) {
        RecClick c;
        if (len != sizeof c) return 0;
        memcpy(&c, b, sizeof c);
        if (c.player >= REC_PLAYERS || !rec_click_ok(&c)) return 0;
    } else {
        RecSel head;
        unsigned lo, hi;
        long prev = -1;
        if (!rec_sel_head(b, len, w->each, &head) || !rec_sel_whole(&head) ||
            head.player >= REC_PLAYERS)
            return 0;
        if (head.count && (!rec_sel_block((const unsigned char*)w->players[head.player],
                                          w->nslots, &lo, &hi) ||
                           !rec_sel_entries_ok(b + sizeof head, head.count, lo, hi, &prev)))
            return 0;
        memcpy(w->sel[head.player], b + sizeof head, (size_t)head.count * sizeof(RecSelEntry));
        w->nsel[head.player] = head.count;
        w->sel_in[head.player] = 1;
    }
    buf_rec(&w->iter, kind, b, len, NULL, 0);
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
    /* the readers' events flag, by the same predicate (feature_record's INVARIANT); a record that
       sets it and is then refused abandons the file, so setting it first changes nothing */
    if (rec_section_of(kind) == RCS_EVENTS) w->evented = 1;
    switch (kind) {
    case RK_TICK:
        if (len != 4 || w->fbracket || w->ndeaths) return 0;
        if (w->ticked) iteration_done(w);
        if (w->dead) return 1;
        w->evented = 0;
        memcpy(&w->tick, b, 4);
        if (!w->ticked) w->snap_tick = w->tick;
        w->ticked = 1;
        w->iterations++;
        buf_rec(&w->iter, RK_TICK, b, 4, NULL, 0);
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
    case RK_POV:
    case RK_CLICK:
    case RK_SELECT:
        return pov_record(w, kind, b, len);
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
    case RK_MINIMAP: {
        unsigned short mw, mh;
        if (w->ticked || len < REC_MINIMAP_HEAD) return 0;
        memcpy(&mw, b, 2);
        memcpy(&mh, b + 2, 2);
        if (len != REC_MINIMAP_HEAD + (unsigned)mw * mh) return 0;
        block(w, RB_MINIMAP, 0, 0, b, len, 1);
        return 1;
    }
    case RK_END:
        if (len != REC_END_BYTES || w->fbracket || w->ndeaths) return 0;
        memcpy(w->seats, b + 24, sizeof w->seats);
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
    buf_rec(&w->iter, kind, b, len, NULL, 0);
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

static void contributions_flush(Writer* w)
{
    if (!w->contributions.n || w->dead) return;
    block(w, RB_CONTRIBUTIONS, 0, 0,
          w->contributions.p, w->contributions.n, 1);
    w->contributions.n = 0;
}

static void contribution(Writer* w, const RecMpRecorded* record)
{
    if (!tagpu_recmp_history_record(w->mp_history, &record->where, &record->packet.h,
        record->packet.body, record->packet.h.bytes - sizeof record->packet.h)) {
        fail(w, "malformed contribution journal or proof tail");
        return;
    }
    w->mp=*tagpu_recmp_history_ledger(w->mp_history);
    buf_put(&w->contributions, &record->where, sizeof record->where);
    buf_put(&w->contributions, &record->packet, record->packet.h.bytes);
    if (w->contributions.oom) fail(w, "no memory for contribution block");
    else if (w->contributions.n >= STREAM_BYTES) contributions_flush(w);
}

static void contributions_poll(Writer* w)
{
    RecMpRecorded record;
    unsigned count = 0;
    int taken = 0;
    if (!w->mp_in || w->dead) return;
    while (!w->dead && count++ < 512 && (taken = tagpu_recmp_record_take(
        tagpu_recmp_file_journal(w->ring->mp), &record)) > 0) contribution(w, &record);
    if (w->dead) return;
    if (count <= 512 && taken < 0) fail(w, "local contribution journal failed");
}

/* The game has published its final ring record. Only owned copied data survives;
   the channel's sole receive consumer transfers at its producer's end publication. */
static void contributions_finish(Writer* w)
{
    RecMpFile* f = w->ring->mp;
    if (!f) return;
    while (!tagpu_recmp_file_ended(f)) WaitForSingleObject(w->ring->wake, REC_WRITER_POLL_MS);
    if (!w->mp_in || tagpu_recmp_file_ended(f) != 1) {
        if (!w->dead) fail(w, "local multiplayer recording stopped before channel hand-over");
        return;
    }
    while (!w->dead) {
        RecMpRecorded record = {0};
        int taken, complete;
        contributions_poll(w);
        if (w->dead) break;
        if (!tagpu_recmp_record_done(tagpu_recmp_file_journal(f))) continue;
        record.where.phase = RECMP_RECORD_PROOF;
        while ((taken = tagpu_recmp_file_proof(f, &record.packet)) > 0 && !w->dead)
            contribution(w, &record);
        if (w->dead) break;
        if (taken == -2) { fail(w, "contribution receive queue filled before the file drained it"); break; }
        if (taken < 0) { fail(w, "multiplayer channel failed before file completion"); break; }
        complete = tagpu_recmp_file_complete(f, &w->mp, &w->mp_end);
        if (complete < 0) { fail(w, "persisted contribution prefixes do not cover application cuts"); break; }
        if (complete) {
            if (!tagpu_recmp_history_footer(w->mp_history,&w->mp_end)) {
                fail(w,"communication receipt lacks its byte-exact original contribution"); break;
            }
            contributions_flush(w); break;
        }
        WaitForSingleObject(w->ring->wake, REC_WRITER_POLL_MS);
    }
}

static void finish(Writer* w)
{
    Buf e = { 0 };
    unsigned i;
    if (!w->dead && w->header) {
        stream_flush(w, 0);
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
        tlv(&e, RT_GAME_END, w->seats, 4);
        for (i = 0; i < REC_PLAYERS; i++)
            if (w->seats[4 + 12 * i + 1] != REC_RESULT_NONE)
                tlv(&e, RT_SEAT_END, w->seats + 4 + 12 * i, 12);
        if (w->mp_in) tlv(&e, RT_MP_END, &w->mp_end, sizeof w->mp_end);
        tlv_u32(&e, RT_CODEC_DECLINED, w->codec.declined);
        tlv_u32(&e, RT_CODEC_MISMATCH, w->codec.mismatched);
        if (e.oom) fail(w, "no memory for the end block");
        else block(w, RB_END, w->tick, w->tick, e.p, e.n, 0);
    }
    if (!w->dead && w->file != INVALID_HANDLE_VALUE) {
        unsigned corr = w->corr_more;
        LARGE_INTEGER f;
        LONGLONG hz;
        for (i = 0; i < w->ncorr; i++) corr += w->corr[i].count;
        CloseHandle(w->file);
        hz = QueryPerformanceFrequency(&f) && f.QuadPart > 0 ? f.QuadPart : 1;
        tagpu_logf("recorder: writer: %s closed: %u iterations to GameTime %u, %u corrections, "
                   "stream %u KB of records written as %u KB; %u blocks: %u sectioned, %u "
                   "declined, %u MISMATCHED; the codec cost %u ms, at most %u ms for one "
                   "block; the ring held %u KB at most; %u snapshots, %u of them keys, packed "
                   "in %u ms, at most %u ms for one", w->path, w->iterations, w->tick, corr,
                   (unsigned)(w->raw >> 10), (unsigned)(w->packed >> 10), w->nstream,
                   w->codec.sectioned, w->codec.declined, w->codec.mismatched,
                   (unsigned)(w->codec_qpc * 1000 / hz), (unsigned)(w->codec_max * 1000 / hz),
                   w->ring_high >> 10, w->nsnap, w->codec_on ? w->codec.keys : w->nsnap,
                   (unsigned)(w->snap_qpc * 1000 / hz), (unsigned)(w->snap_max * 1000 / hz));
    }
    recfile_unlock(&w->lock);
    free(e.p);
    reccodec_stream_end(&w->codec);
    ZSTD_freeCCtx((ZSTD_CCtx*)w->cctx);
    free(w->pk.p);
    free(w->stream.p);
    free(w->iter.p);
    free(w->exact.p);
    reclossy_free(w->lossy);
    free(w->snap.p);
    free(w->contributions.p);
    tagpu_recmp_history_end(w->mp_history);
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
    free(w->wbase);
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
        if (g->fork.file) {             /* THE FORK'S HANDLE moves to the writer */
            w->file = g->fork.file;
            w->lock = g->fork.lock;
            g->fork.file = NULL;
            g->fork.lock = NULL;
            w->append = 1;
        }
    } else {
        InterlockedExchange(&g->failed, 1);
        tagpu_logf("recorder: writer: no memory: %s is not written", g->path);
        if (g->fork.file) CloseHandle(g->fork.file);
        g->fork.file = NULL;
        recfile_unlock(&g->fork.lock);
    }
    while (!stop) {
        unsigned wp;
        LONG req;
        WaitForSingleObject(g->wake, REC_WRITER_POLL_MS);   /* tagpu_recorder.h, *Waking* */
        req = InterlockedCompareExchange(&g->flush_req, 0, 0);
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
        if (w && !w->dead) {
            if (req != InterlockedCompareExchange(&g->flush_done, 0, 0)) {
                stream_flush(w, 0);
                if (!w->dead) InterlockedExchange(&g->flush_done, req);
            }
        }
        InterlockedExchange(&g->rpub, (LONG)r);
        if (w) contributions_poll(w);
        stop = InterlockedCompareExchange(&g->ended, 0, 0) &&
               wp == (unsigned)InterlockedCompareExchange(&g->wend, 0, 0);
    }
    /* the hand-over (tagpu_recorder.h): the capture's last access to the ring is behind us */
    if (w) {
        contributions_finish(w);
        finish(w);
        if (w->append && !recfile_request(w->path, REC_FILE_COMPACT, g->fork.id, NULL, NULL))
            tagpu_logf("replay: forks %s: compaction skipped: worker unavailable", w->path);
        free(w);
    }
    if (g->mp) {
        while (!tagpu_recmp_file_ended(g->mp)) WaitForSingleObject(g->wake, REC_WRITER_POLL_MS);
        tagpu_recmp_file_free(g->mp);
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
