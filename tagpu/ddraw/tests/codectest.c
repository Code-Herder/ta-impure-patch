/* codectest.c -- the stream codec's self-test (src/tagpu_reccodec.c and its record frames,
   src/tagpu_recframe_*.c), driven by codectest.py.

     codectest transcode IN OUT [--snap-every N] [--skirmish]
                                    every stream block of a recording decoded to its records and
                                    written again through reccodec_stream_block, restarting where
                                    the writer restarts; a line per block (raw, section and packed
                                    bytes, milliseconds) and the outcomes. With N, only every Nth
                                    snapshot of a segment is kept (0: none), and the stream
                                    restarts only after those: the file a writer whose snapshot
                                    interval is N times IN's would have written. With --skirmish,
                                    a skirmish header without RT_HOLD gains one that holds nothing,
                                    refresh REC_REFRESH_TICKS: a label, the blocks are IN's
     codectest lossy IN OUT [--exact]
                                    the writer's path over a recording's exact records: each
                                    iteration through the lossy filter (src/tagpu_reclossy.c) with
                                    the build's hold table, refreshes where the writer makes them,
                                    the stream blocks cut where it cuts them and coded by its codec,
                                    IN's snapshots kept where a writer would take one (a refresh
                                    SNAPSHOT_TICKS on), the filter told of each before it, and the
                                    rest dropped; with --exact, each
                                    stream block followed by its exact records (RB_EXACT). Prints
                                    the iterations whose records differ from IN's
     codectest decode IN OUT        every stream block's records, C, framed for codectest.py; a
                                    line per block with its milliseconds
     codectest outcomes             synthetic payloads of units and projectiles that must be
                                    SECTIONS, DECLINED (a unit delta naming an unchanged dword; a
                                    correction on a delta's slot; a failed allocation; a
                                    projectile slot named twice, two RK_PROJ_HEAD, an extent that
                                    shrinks, a projectile delta naming an unchanged dword or with
                                    a pad that is not zero) and MISMATCH (the check decoder
                                    restarted from other units, from another pool), each followed
                                    by a restarting SECTIONS block; each record frame's own
                                    (outcomes_cells, _orders, _events, _state); zstd frames
                                    past and at the 2^27 window bound; and the ledger rule's
                                    single-precision sum; the lossy filter's own (an empty table
                                    copies, a refresh writes exactly the differences, the equality
                                    check, malformed iterations refused, RT_HOLD's bounds, the
                                    refresh grid and the refresh before a GameTime); the spray
                                    sessions' (a spray implied plain and following its unit,
                                    written after a miss and twice in one iteration, a session
                                    closed, a snapshot's iteration, and what a reader refuses)
     codectest sprays IN            the Windows build's only: the DLL's own reader
                                    (src/tagpu_recreader.c) over IN's first timeline, each
                                    iteration's sprays -- the stream's and the ones its spray
                                    sessions imply -- against IN's exact stream (RB_EXACT, the test
                                    lever tagpu_rec.exact), the same bytes as a multiset and each
                                    unit's in order: read in order from the start, and after a
                                    seek to each snapshot's GameTime, to the one after it, read on
                                    to the next snapshot, and to one 151 after it
     codectest fuzz IN SEED N [CASES] [--section NAME]
                                    N mutations of the recording's sectioned blocks (flags, the
                                    head, section lengths, kinds, the unit frame's gaps, masks,
                                    ids and columns, the pool frame's control values, codes and
                                    columns, the record frames' column tables and columns,
                                    RCS_OTHER's bodies, the header's codec entries, truncations),
                                    each decoded from a copy of the state the block starts from;
                                    each block itself, decoded from the same copy, must give the
                                    reading's records (exit 1 naming the block when not); with CASES, every case and its result for codectest.py's
                                    differential; with --section, only the mutations of section
                                    NAME (kinds, units, proj, cells, orders, events, other,
                                    state)
     codectest sin                  the sine table, for codectest.py's check against its formula

   IN is a recording of format 16, or, for transcode and lossy, one of format 12 or later with no
   sectioned block, which they write as the current format, the header's other entries kept. Exit
   0 when every check holds, 1 when one fails, 2 when a file cannot be read. The codec is included
   rather than linked: the fuzz copies a decoder's state, which the codec's interface keeps opaque;
   the filter is linked, and in the Windows build the DLL's reader too, over the codec included
   here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/tagpu_reccodec.c"
#include "tagpu_reclossy.h"
#include "tagpu_rechold.h"
#include "tagpu_contest.h"
#ifdef _WIN32
#include "tagpu_recreader.h"
#include "tagpu_log.h"
#endif

#define BLOCK_MAX (64u << 20)

static uint32_t crc_table[256];

static void crc_init(void)
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) {
        for (c = i, k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32_of(const unsigned char* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint64_t fnv64(const unsigned char* p, size_t n)
{
    uint64_t h = UINT64_C(14695981039346656037);
    while (n--) h = (h ^ *p++) * UINT64_C(1099511628211);
    return h;
}

static double ms_since(clock_t t0) { return (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC; }

static unsigned char* read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    unsigned char* d;
    long len;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (unsigned char*)malloc(len > 0 ? (size_t)len : 1);
    if (!d || fread(d, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return d;
}

/* ---- a recording, block by block ---------------------------------------------------------------- */

typedef struct Reader {
    const unsigned char* d;
    size_t n, at;
    unsigned ordinal;
    RecBlock b;
    const unsigned char* body;
    unsigned char* raw;           /* the unpacked bytes, when packed */
    const unsigned char* bytes;   /* the block's bytes: the body if stored, else raw */
    void* dctx;
    char why[256];
} Reader;

/* the next block, checked as the DLL's reader checks it: 1, 0 at the end, -1 refused (why) */
static int next_block(Reader* r)
{
    unsigned allowed;
    if (r->at == r->n) return 0;
    if (r->n - r->at < 32) {
        snprintf(r->why, sizeof r->why, "a block header cut short at byte %lu", (unsigned long)r->at);
        return -1;
    }
    memcpy(&r->b, r->d + r->at, 32);
    if (r->b.magic != REC_BLOCK_MAGIC || crc32_of(r->d + r->at, 28) != r->b.crc_head) {
        snprintf(r->why, sizeof r->why, "a torn block header at byte %lu", (unsigned long)r->at);
        return -1;
    }
    if (r->b.packed > r->n - r->at - 32 || r->b.raw > BLOCK_MAX || r->b.packed > BLOCK_MAX) {
        snprintf(r->why, sizeof r->why, "a block cut short at byte %lu", (unsigned long)r->at);
        return -1;
    }
    r->body = r->d + r->at + 32;
    if (crc32_of(r->body, r->b.packed) != r->b.crc_body) {
        snprintf(r->why, sizeof r->why, "a block failing its CRC at byte %lu", (unsigned long)r->at);
        return -1;
    }
    allowed = RBF_STORED | (r->b.kind == RB_STREAM ? RBF_SECTIONS | RBF_FRAME_RESET | RBF_REFRESH : 0);
    if ((r->b.flags & ~allowed) || ((r->b.flags & RBF_FRAME_RESET) && !(r->b.flags & RBF_SECTIONS)) ||
        ((r->b.kind == RB_HEADER || r->b.kind == RB_END || r->b.kind == RB_TIMELINES) &&
         !(r->b.flags & RBF_STORED))) {
        snprintf(r->why, sizeof r->why, "block flags %#x on kind %u at byte %lu", r->b.flags, r->b.kind,
                 (unsigned long)r->at);
        return -1;
    }
    if (r->b.flags & RBF_STORED) {
        if (r->b.raw != r->b.packed) {
            snprintf(r->why, sizeof r->why, "a stored block of %lu bytes, not %lu, at byte %lu",
                     (unsigned long)r->b.packed, (unsigned long)r->b.raw, (unsigned long)r->at);
            return -1;
        }
        r->bytes = r->body;
    } else {
        free(r->raw);
        r->raw = (unsigned char*)malloc(r->b.raw ? r->b.raw : 1);
        if (!r->raw || !reccodec_unpack(r->dctx, r->raw, r->b.raw, r->body, r->b.packed)) {
            snprintf(r->why, sizeof r->why, "a block that is not one zstd frame of %lu bytes at byte %lu",
                     (unsigned long)r->b.raw, (unsigned long)r->at);
            return -1;
        }
        r->bytes = r->raw;
    }
    r->at += 32 + r->b.packed;
    r->ordinal++;
    return 1;
}

typedef struct Header {
    unsigned format, mode, slots, nproj, gw, gh, nwrecks, has_hold;
    const unsigned char* e[3];      /* RT_CODEC, RT_PHYSICS, RT_AIR */
    unsigned ne[3];
} Header;

static int header_of(const unsigned char* p, size_t n, Header* h, char* why, unsigned cap)
{
    size_t at = 0;
    memset(h, 0, sizeof *h);
    while (n - at >= 4) {
        unsigned tag = get16(p + at), len = get16(p + at + 2), k;
        const unsigned char* b = p + at + 4;
        if (len > n - at - 4) REFUSE("a header entry cut short");
        if (tag == RT_FORMAT && len == 4) h->format = get32(b);
        if (tag == RT_MODE && len == 4) h->mode = get32(b);
        if (tag == RT_HOLD) h->has_hold = 1;
        if (tag == RT_SLOTS && len == 4) h->slots = get32(b);
        if (tag == RT_PROJECTILES && len == 4) h->nproj = get32(b);
        if (tag == RT_FEATURES && len == 20) {
            h->gw = get32(b);
            h->gh = get32(b + 4);
            h->nwrecks = get32(b + 8);
        }
        k = tag == RT_CODEC ? 0 : tag == RT_PHYSICS ? 1 : tag == RT_AIR ? 2 : 3;
        if (k < 3) {
            if (h->e[k]) REFUSE("header tag %u twice", tag);
            h->e[k] = b;
            h->ne[k] = len;
        }
        at += (4 + (size_t)len + 3) & ~(size_t)3;
    }
    /* an older format is read for its records alone (reading_next) */
    if (h->format < 12 || h->format > REC_FORMAT)
        REFUSE("format %u, not 12 to %u", h->format, REC_FORMAT);
    return 1;
}

/* the reader's copies of the units, of the projectile pool and of the feature grid */
typedef struct World {
    unsigned nslots, nproj, pcount, phw;
    uint32_t* unit;
    unsigned char* live;
    unsigned char* pool;          /* nproj images; zero from phw on */
    unsigned gw, gh;
    unsigned char* grid;          /* gw * gh cells; NULL for a grid past the codec's bounds,
                                     which no sectioned block can follow */
} World;

static void world_reset(World* w, unsigned nslots, unsigned nproj, unsigned gw, unsigned gh)
{
    free(w->unit);
    free(w->live);
    free(w->pool);
    free(w->grid);
    w->nslots = nslots;
    w->nproj = nproj;
    w->pcount = w->phw = 0;
    w->unit = (uint32_t*)calloc(nslots, IMG);
    w->live = (unsigned char*)calloc(nslots, 1);
    w->pool = (unsigned char*)calloc(nproj ? nproj : 1, PIMG);
    w->gw = gw;
    w->gh = gh;
    w->grid = gw >= 1 && gh >= 1 && (uint64_t)gw * gh <= RECC_GRID_MAX
                  ? (unsigned char*)calloc((size_t)gw * gh, REC_CELL_BYTES) : NULL;
}

static void world_free(World* w)
{
    free(w->unit);
    free(w->live);
    free(w->pool);
    free(w->grid);
    memset(w, 0, sizeof *w);
}

/* the world a reset hands the record frames (reccodec_restart) */
static RecFrameWorld world_view(const World* w)
{
    RecFrameWorld v;
    v.gw = w->gw;
    v.gh = w->gh;
    v.grid = w->grid;
    return v;
}

/* an image onto the world's grid when it is within tagpu_recreader.c's cells_ok, which a reader
   that refuses one would stop at: such a record is skipped here */
static void world_cells(World* w, const unsigned char* b, unsigned len)
{
    unsigned x, z, n;
    if (!w->grid || len < 8) return;
    x = get16(b);
    z = get16(b + 2);
    n = get16(b + 4);
    if (get16(b + 6) || n < 1 || n > REC_CELLS_MAX || len != 8 + n * REC_CELL_BYTES ||
        x + n > w->gw || z >= w->gh)
        return;
    memcpy(w->grid + ((size_t)z * w->gw + x) * REC_CELL_BYTES, b + 8, (size_t)n * REC_CELL_BYTES);
}

/* a projectile record onto the world's pool by tagpu_recreader.c's rules, which a reader that
   refuses one would stop at: such a record is skipped here */
static void world_proj(World* w, unsigned kind, const unsigned char* b, unsigned len)
{
    unsigned id = len >= 2 ? get16(b) : 0, i, n = 0, k;
    if (kind == RK_PROJ_HEAD && len == 8) {
        uint32_t v0 = get32(b), v1 = get32(b + 4);
        if (v0 > v1 || v1 > w->nproj) return;
        if (v1 < w->phw) memset(w->pool + (size_t)v1 * PIMG, 0, (size_t)(w->phw - v1) * PIMG);
        w->pcount = v0;
        w->phw = v1;
    } else if (kind == RK_PROJ && len == 4 + PIMG && !b[2] && !b[3] && id < w->phw) {
        memcpy(w->pool + (size_t)id * PIMG, b + 4, PIMG);
    } else if (kind == RK_PROJ_DELTA && len >= REC_PROJ_DELTA_HEAD && id < w->phw) {
        for (i = 0; i < 32; i++) n += bit_of(b + 2, i);
        for (i = PDW; i < 32; i++)
            if (bit_of(b + 2, i)) return;
        if (len != REC_PROJ_DELTA_HEAD + 4 * n) return;
        for (i = 0, k = 0; i < PDW; i++)
            if (bit_of(b + 2, i)) memcpy(w->pool + (size_t)id * PIMG + 4 * i, b + REC_PROJ_DELTA_HEAD + 4 * k++, 4);
    }
}

/* the records onto the world; an RK_CELLS is an image, onto the grid, when no RCS_EVENTS record
   came before it in its iteration (the reader's rule, tagpu_recreader.c `apply`) */
static void world_apply(World* w, const unsigned char* pl, size_t n)
{
    size_t at = 0;
    int events = 0;
    while (n - at >= 4) {
        unsigned kind = get16(pl + at), len = get16(pl + at + 2), s, i, k, nb, ok;
        size_t rl = ((size_t)4 + len + 3) & ~(size_t)3;
        const unsigned char* b = pl + at + 4;
        if (rl > n - at) break;
        if (kind == RK_TICK) events = 0;
        if (kind == RK_CELLS && !events) world_cells(w, b, len);
        if (rec_section_of(kind) == RCS_EVENTS) events = 1;
        if (kind == RK_PROJ_HEAD || kind == RK_PROJ || kind == RK_PROJ_DELTA) world_proj(w, kind, b, len);
        s = len >= 2 ? get16(b) : 0;
        if (s >= 1 && s < w->nslots) {
            if (kind == RK_UNIT && len == 4 + IMG) {
                memcpy(w->unit + (size_t)s * NDW, b + 4, IMG);
                w->live[s] = 1;
            } else if (kind == RK_UNIT_GONE && len == 4) {
                w->live[s] = 0;
            } else if (kind == RK_UNIT_DELTA && len >= REC_UNIT_DELTA_HEAD && w->live[s]) {
                for (i = 0, nb = 0, ok = 1; i < REC_UNIT_BITS * 8; i++)
                    if (bit_of(b + 2, i)) {
                        ok &= i < NDW;
                        nb++;
                    }
                if (ok && len == REC_UNIT_DELTA_HEAD + 4 * nb)
                    for (i = 0, k = 0; i < NDW; i++)
                        if (bit_of(b + 2, i)) memcpy(w->unit + (size_t)s * NDW + i, b + REC_UNIT_DELTA_HEAD + 4 * k++, 4);
            }
        }
        at += rl;
    }
}

/* the live slots, the pool and the grid of the codec's copies against the world's */
static int world_same(const World* w, const RecCodec* c)
{
    unsigned s, i;
    RecFrameWorld fw;
    reccodec_world(c, &fw);
    if (!fw.grid || !w->grid || fw.gw != w->gw || fw.gh != w->gh ||
        memcmp(fw.grid, w->grid, (size_t)w->gw * w->gh * REC_CELL_BYTES))
        return 0;
    for (s = 1; s < w->nslots; s++) {
        if (!w->live[s] != !c->live[s]) return 0;
        if (w->live[s] && memcmp(w->unit + (size_t)s * NDW, c->unit + (size_t)s * NDW, IMG)) return 0;
    }
    if (w->pcount != c->pcount || w->phw != c->phw) return 0;
    for (i = 0; i < w->phw * PDW; i++)
        if (get32(w->pool + 4 * (size_t)i) != c->pool[i]) return 0;
    return 1;
}

static void put_block(FILE* f, unsigned kind, unsigned flags, uint32_t first, uint32_t last,
                      uint32_t raw, const unsigned char* body, size_t bn)
{
    RecBlock b;
    memset(&b, 0, sizeof b);
    b.magic = REC_BLOCK_MAGIC;
    b.kind = (uint16_t)kind;
    b.flags = (uint16_t)flags;
    b.first = first;
    b.last = last;
    b.raw = raw;
    b.packed = (uint32_t)bn;
    b.crc_body = crc32_of(body, bn);
    b.crc_head = crc32_of((const unsigned char*)&b, 28);
    fwrite(&b, 1, sizeof b, f);
    fwrite(body, 1, bn, f);
}

/* The state a reading carries from block to block: the segment's params, the world, the decoder. */
typedef struct Reading {
    Reader r;
    Header h;
    RecCodecParams p;
    World w;
    RecCodec* dec;
    RecBytes pay;
    int in_segment;
} Reading;

static int reading_open(Reading* g, const char* path)
{
    memset(g, 0, sizeof *g);
    crc_init();
    g->r.d = read_file(path, &g->r.n);
    g->r.dctx = ZSTD_createDCtx();
    if (!g->r.d || g->r.n < 8 || memcmp(g->r.d, REC_MAGIC, 8)) {
        fprintf(stderr, "codectest: %s: not a recording\n", path);
        return 0;
    }
    g->r.at = 8;
    return 1;
}

static void reading_close(Reading* g)
{
    free((void*)g->r.d);
    free(g->r.raw);
    ZSTD_freeDCtx((ZSTD_DCtx*)g->r.dctx);
    world_free(&g->w);
    reccodec_free(g->dec);
    rb_free(&g->pay);
    memset(g, 0, sizeof *g);
}

/* One block: 1 and, for a stream block, its records in g->pay; 0 at the end; -1 refused. */
static int reading_next(Reading* g, double* ms)
{
    int k = next_block(&g->r);
    *ms = 0;
    if (k <= 0) return k;
    if (g->r.b.kind == RB_HEADER) {
        if (!header_of(g->r.bytes, g->r.b.raw, &g->h, g->r.why, sizeof g->r.why)) return -1;
        if (!reccodec_params(&g->p, g->h.slots, g->h.nproj, g->h.gw, g->h.gh, g->h.nwrecks, g->h.e[0],
                             g->h.ne[0], g->h.e[1], g->h.ne[1], g->h.e[2], g->h.ne[2], g->r.why,
                             sizeof g->r.why))
            return -1;
        if (g->dec && (g->dec->nslots != g->h.slots || g->dec->nproj != g->h.nproj ||
                       g->dec->gw != g->h.gw || g->dec->gh != g->h.gh || g->dec->nwrecks != g->h.nwrecks)) {
            snprintf(g->r.why, sizeof g->r.why, "a segment of another shape (%u slots, %u projectiles, a "
                     "%u x %u grid, %u wrecks) after %u, %u, %u x %u and %u", g->h.slots, g->h.nproj,
                     g->h.gw, g->h.gh, g->h.nwrecks, g->dec->nslots, g->dec->nproj, g->dec->gw,
                     g->dec->gh, g->dec->nwrecks);
            return -1;
        }
        world_reset(&g->w, g->h.slots, g->h.nproj, g->h.gw, g->h.gh);
        /* made for the first segment with a codec, whose sizes every segment's equal (the DLL's
           reader's shape_differs) */
        if (!g->dec && g->p.usable) g->dec = reccodec_new(&g->p);
        if ((g->p.usable && (!g->dec || !g->w.grid)) || !g->w.unit || !g->w.live || !g->w.pool) {
            snprintf(g->r.why, sizeof g->r.why, "no memory");
            return -1;
        }
        if (g->dec) reccodec_invalidate(g->dec);
        g->in_segment = 1;
        return 1;
    }
    if (g->r.b.kind != RB_STREAM) return 1;
    if (g->r.b.flags & RBF_SECTIONS) {
        clock_t t0 = clock();
        /* an older format's codec is not this one: its containers would decode to other records */
        if (g->h.format != REC_FORMAT) {
            snprintf(g->r.why, sizeof g->r.why, "a sectioned block of format %u: convert a "
                     "records-only file", g->h.format);
            return -1;
        }
        if (!g->p.usable) {
            snprintf(g->r.why, sizeof g->r.why, "a sectioned block in a segment without RT_CODEC and RT_PHYSICS");
            return -1;
        }
        if (g->r.b.flags & RBF_FRAME_RESET) {
            RecFrameWorld v = world_view(&g->w);
            reccodec_restart(g->dec, &g->p, g->w.unit, g->w.live, g->w.pool, g->w.pcount, g->w.phw, &v);
        }
        if (!reccodec_decode(g->dec, g->r.bytes, g->r.b.raw, &g->pay, g->r.why, sizeof g->r.why)) return -1;
        *ms = ms_since(t0);
    } else {
        if (g->dec) reccodec_invalidate(g->dec);
        g->pay.n = 0;
        g->pay.oom = 0;
        rb_put(&g->pay, g->r.bytes, g->r.b.raw);
    }
    world_apply(&g->w, g->pay.p, g->pay.n);
    return 1;
}

/* ---- transcode, decode ------------------------------------------------------------------------ */

static void tlv_put(RecBytes* b, unsigned tag, const void* p, unsigned n)
{
    static const unsigned char zero[4];
    unsigned char h[4] = { (unsigned char)tag, (unsigned char)(tag >> 8), (unsigned char)n,
                           (unsigned char)(n >> 8) };
    rb_put(b, h, 4);
    rb_put(b, p, n);
    rb_put(b, zero, ((n + 3) & ~3u) - n);
}

/* RT_HOLD's bytes: `masks` the build's writer's -- its hold table and the flags every skirmish
   recording carries, its cells held and its sprays as sessions --
   else a table that holds nothing */
static void hold_bytes(unsigned char* hold, int masks)
{
    unsigned i;
    memset(hold, 0, REC_HOLD_BYTES);
    put32(hold, REC_REFRESH_TICKS);
    if (!masks) return;
    put32(hold + 4, REC_SKIRMISH_HOLDS);
    for (i = 0; i < 32; i++) {
        const char* x = TAGPU_REPLAY_HOLD_SHA256 + 2 * i;
        unsigned hi = x[0] <= '9' ? (unsigned)(x[0] - '0') : (unsigned)(x[0] - 'a' + 10);
        unsigned lo = x[1] <= '9' ? (unsigned)(x[1] - '0') : (unsigned)(x[1] - 'a' + 10);
        hold[8 + i] = (unsigned char)(hi << 4 | lo);
    }
    memcpy(hold + 40, REC_HOLD_UNIT, REC_UNIT_IMAGE);
    memcpy(hold + 40 + REC_UNIT_IMAGE, REC_HOLD_NODE, REC_ORDER_IMAGE);
}

/* A header's entries as the current format writes them: RT_FORMAT REC_FORMAT, every other entry
   kept but RT_HOLD, and `hold`'s RT_HOLD after them when it is not NULL. */
static void header_out(RecBytes* out, const unsigned char* p, size_t n, const unsigned char* hold)
{
    size_t at = 0;
    out->n = 0;
    while (n - at >= 4) {
        unsigned tag = get16(p + at), len = get16(p + at + 2);
        size_t step = (4 + (size_t)len + 3) & ~(size_t)3;
        if (len > n - at - 4) break;
        if (tag == RT_FORMAT && len == 4) {
            unsigned char f[4];
            put32(f, REC_FORMAT);
            tlv_put(out, RT_FORMAT, f, 4);
        } else if (tag != RT_HOLD) {
            rb_put(out, p + at, step < n - at ? step : n - at);
        }
        at += step;
    }
    if (hold) tlv_put(out, RT_HOLD, hold, REC_HOLD_BYTES);
}

static int cmd_transcode(const char* in, const char* outp, int every, int skirmish)
{
    Reading g;
    RecStream st;
    FILE* out;
    int k, on = 0, bad = 0;
    unsigned nsnap = 0;
    double ms, tot = 0, worst = 0;
    size_t raw = 0, written = 0, secbytes = 0, unitbytes = 0, poolbytes = 0;
    unsigned nblk = 0;
    memset(&st, 0, sizeof st);
    if (!reading_open(&g, in)) return 2;
    out = fopen(outp, "wb");
    if (!out) return 2;
    fwrite(REC_MAGIC, 1, 8, out);
    printf("block kind first last raw section units pool packed flags outcome ms\n");
    while ((k = reading_next(&g, &ms)) > 0) {
        RecBlock* b = &g.r.b;
        if (b->kind == RB_HEADER) {
            RecBytes hb;
            unsigned char hold[REC_HOLD_BYTES];
            memset(&hb, 0, sizeof hb);
            if (on) reccodec_stream_end(&st);
            on = g.p.usable && reccodec_stream_begin(&st, &g.p);
            nsnap = 0;
            hold_bytes(hold, 0);
            header_out(&hb, g.r.bytes, b->raw, skirmish && g.h.mode != 3 && !g.h.has_hold ? hold : NULL);
            if (g.h.has_hold) {
                /* the input's own, kept in its place at the end */
                size_t at = 0;
                while (b->raw - at >= 4) {
                    unsigned tag = get16(g.r.bytes + at), len = get16(g.r.bytes + at + 2);
                    if (tag == RT_HOLD && len <= b->raw - at - 4)
                        tlv_put(&hb, RT_HOLD, g.r.bytes + at + 4, len);
                    at += (4 + (size_t)len + 3) & ~(size_t)3;
                }
            }
            put_block(out, b->kind, RBF_STORED, b->first, b->last, (uint32_t)hb.n, hb.p, hb.n);
            rb_free(&hb);
        } else if (b->kind == RB_STREAM && on) {
            unsigned flags;
            const unsigned char* body;
            size_t bn;
            clock_t t0 = clock();
            int o = reccodec_stream_block(&st, g.pay.p, g.pay.n, &flags, &body, &bn);
            double t = ms_since(t0);
            const RecSecHead* sh = (const RecSecHead*)st.sec.p;
            tot += t;
            if (t > worst) worst = t;
            raw += g.pay.n;
            written += bn;
            if (o == RECC_SECTIONS) {
                secbytes += st.sec.n;
                unitbytes += sh->len[RCS_UNITS];
                poolbytes += sh->len[RCS_PROJ];
            }
            put_block(out, b->kind, flags | (b->flags & RBF_REFRESH), b->first, b->last,
                      (uint32_t)(o == RECC_SECTIONS ? st.sec.n : g.pay.n), body, bn);
            printf("%u stream %lu %lu %lu %lu %lu %lu %lu %#x %s %.2f\n", g.r.ordinal, (unsigned long)b->first,
                   (unsigned long)b->last, (unsigned long)g.pay.n,
                   (unsigned long)(o == RECC_SECTIONS ? st.sec.n : 0),
                   (unsigned long)(o == RECC_SECTIONS ? sh->len[RCS_UNITS] : 0),
                   (unsigned long)(o == RECC_SECTIONS ? sh->len[RCS_PROJ] : 0), (unsigned long)bn, flags,
                   o == RECC_SECTIONS ? "sections" : o == RECC_DECLINED ? "DECLINED" : "MISMATCH", t);
            if (o != RECC_SECTIONS) bad = 1;
            if (!world_same(&g.w, st.enc)) {
                printf("codectest: the encoder's copy is not the world's (units, pool or grid) after "
                       "block %u\n", g.r.ordinal);
                bad = 1;
            }
            nblk++;
        } else if (b->kind == RB_END) {
            RecBytes e;
            uint32_t v;
            memset(&e, 0, sizeof e);
            rb_put(&e, g.r.bytes, b->raw);
            v = (uint32_t)(RT_CODEC_DECLINED | 4u << 16);
            rb_put(&e, &v, 4);
            rb_put(&e, &st.declined, 4);
            v = (uint32_t)(RT_CODEC_MISMATCH | 4u << 16);
            rb_put(&e, &v, 4);
            rb_put(&e, &st.mismatched, 4);
            put_block(out, b->kind, RBF_STORED, b->first, b->last, (uint32_t)e.n, e.p, e.n);
            rb_free(&e);
        } else if (b->kind == RB_TIMELINES || b->kind == RB_STREAM) {
            put_block(out, b->kind, RBF_STORED | (b->flags & RBF_REFRESH), b->first, b->last, b->raw,
                      g.r.bytes, b->raw);
        } else if (b->kind == RB_SNAPSHOT && every >= 0 && (every == 0 || ++nsnap % (unsigned)every)) {
            continue;
        } else {
            RecBytes pk;
            int packed;
            memset(&pk, 0, sizeof pk);
            packed = on && reccodec_pack(st.cctx, &pk, g.r.bytes, b->raw);
            put_block(out, b->kind, packed ? 0 : RBF_STORED, b->first, b->last, b->raw,
                      packed ? pk.p : g.r.bytes, packed ? pk.n : b->raw);
            rb_free(&pk);
            if (b->kind == RB_SNAPSHOT) st.reset = 1;
        }
    }
    fclose(out);
    if (k < 0) {
        printf("codectest: %s: %s\n", in, g.r.why);
        reading_close(&g);
        return 2;
    }
    reading_close(&g);
    printf("summary: %u stream blocks: %u sectioned, %u declined, %u mismatched; records %lu bytes, "
           "containers %lu (unit frames %lu, pool frames %lu), written %lu; encode+check %.0f ms, at most "
           "%.2f ms a block\n",
           nblk, st.sectioned, st.declined, st.mismatched, (unsigned long)raw, (unsigned long)secbytes,
           (unsigned long)unitbytes, (unsigned long)poolbytes, (unsigned long)written, tot, worst);
    if (on) reccodec_stream_end(&st);
    return bad || st.declined || st.mismatched ? 1 : 0;
}

static int cmd_decode(const char* in, const char* outp)
{
    Reading g;
    FILE* out;
    int k;
    double ms, tot = 0, run = 0, worst_run = 0, worst = 0;
    unsigned nblk = 0, sect = 0;
    if (!reading_open(&g, in)) return 2;
    out = fopen(outp, "wb");
    if (!out) return 2;
    while ((k = reading_next(&g, &ms)) > 0) {
        RecBlock* b = &g.r.b;
        uint32_t head[6];
        if (b->kind != RB_STREAM) {
            if (b->kind == RB_SNAPSHOT || b->kind == RB_HEADER) run = 0;
            continue;
        }
        if (b->flags & RBF_FRAME_RESET) run = 0;
        run += ms;
        tot += ms;
        if (ms > worst) worst = ms;
        if (run > worst_run) worst_run = run;
        sect += (b->flags & RBF_SECTIONS) != 0;
        head[0] = g.r.ordinal;
        head[1] = b->flags;
        head[2] = b->first;
        head[3] = b->last;
        head[4] = (uint32_t)g.pay.n;
        head[5] = 0;
        fwrite(head, 1, sizeof head, out);
        fwrite(g.pay.p, 1, g.pay.n, out);
        printf("%u %lu %lu %.2f\n", g.r.ordinal, (unsigned long)b->first, (unsigned long)b->last, ms);
        nblk++;
    }
    fclose(out);
    if (k < 0) printf("codectest: %s: %s\n", in, g.r.why);
    reading_close(&g);
    if (k < 0) return 2;
    printf("summary: %u stream blocks, %u sectioned; decode %.0f ms, at most %.2f ms a block, %.0f ms "
           "for the longest run from a restart\n", nblk, sect, tot, worst, worst_run);
    return 0;
}

/* ---- lossy: the writer's path offline ------------------------------------------------------------ */

#define LOSSY_STREAM_BYTES   (2u << 20)   /* tagpu_recwriter.c's STREAM_BYTES, STREAM_TICKS and */
#define LOSSY_STREAM_TICKS   30           /* SNAPSHOT_TICKS                                     */
#define LOSSY_SNAPSHOT_TICKS 1800

/* the writer's exact copy of the units, the lists and the grid, built from the exact records as
   its apply builds it: the grid from the cells an iteration carries before its first event, the
   images (tagpu_recwriter.c, feature_record) */
typedef struct Exact {
    unsigned nslots;
    unsigned char* units;
    unsigned char* live;
    RecLossyList* lists;
    unsigned gw, gh;
    unsigned char* grid;
    unsigned pslot, plist, ptotal, pnext;     /* a list whose records have begun */
} Exact;

static void exact_free(Exact* e)
{
    unsigned i;
    for (i = 0; e->lists && i < e->nslots * 2; i++) free(e->lists[i].img);
    free(e->lists);
    free(e->units);
    free(e->live);
    free(e->grid);
    memset(e, 0, sizeof *e);
}

static int exact_reset(Exact* e, unsigned nslots, unsigned gw, unsigned gh)
{
    exact_free(e);
    e->nslots = nslots;
    e->units = (unsigned char*)calloc(nslots, IMG);
    e->live = (unsigned char*)calloc(nslots, 1);
    e->lists = (RecLossyList*)calloc((size_t)nslots * 2, sizeof *e->lists);
    e->gw = gw;
    e->gh = gh;
    if (gw && gh) e->grid = (unsigned char*)calloc((size_t)gw * gh, REC_CELL_BYTES);
    return e->units && e->live && e->lists && (!gw || !gh || e->grid);
}

/* one iteration's records onto the copy: 0 for a record the writer would have refused */
static int exact_apply(Exact* e, const unsigned char* pl, size_t n)
{
    size_t at = 0;
    int evented = 0;
    while (n - at >= 4) {
        unsigned kind = get16(pl + at), len = get16(pl + at + 2), s, i, k, nb;
        const unsigned char* b = pl + at + 4;
        at += ((size_t)4 + len + 3) & ~(size_t)3;
        s = len >= 2 ? get16(b) : 0;
        if (rec_section_of(kind) == RCS_EVENTS) evented = 1;
        if (kind == RK_CELLS && !evented && e->grid) {
            unsigned x, z, c;
            if (len < 8) return 0;
            x = get16(b);
            z = get16(b + 2);
            c = get16(b + 4);
            if (!c || c > REC_CELLS_MAX || get16(b + 6) || len != 8u + c * REC_CELL_BYTES ||
                x + c > e->gw || z >= e->gh)
                return 0;
            memcpy(e->grid + ((size_t)z * e->gw + x) * REC_CELL_BYTES, b + 8,
                   (size_t)c * REC_CELL_BYTES);
            continue;
        }
        if (kind != RK_UNIT && kind != RK_UNIT_DELTA && kind != RK_UNIT_GONE && kind != RK_ORDERS)
            continue;
        if (s < 1 || s >= e->nslots) return 0;
        if (kind == RK_UNIT) {
            if (len != 4 + IMG) return 0;
            memcpy(e->units + (size_t)s * IMG, b + 4, IMG);
            e->live[s] = 1;
        } else if (kind == RK_UNIT_GONE) {
            e->live[s] = 0;
            e->lists[s * 2].n = e->lists[s * 2 + 1].n = 0;
        } else if (kind == RK_UNIT_DELTA) {
            if (len < REC_UNIT_DELTA_HEAD || !e->live[s]) return 0;
            for (i = 0, nb = 0; i < REC_UNIT_BITS * 8; i++)
                if (bit_of(b + 2, i)) {
                    if (i >= NDW) return 0;
                    nb++;
                }
            if (len != REC_UNIT_DELTA_HEAD + 4 * nb) return 0;
            for (i = 0, k = 0; i < NDW; i++)
                if (bit_of(b + 2, i))
                    memcpy(e->units + (size_t)s * IMG + 4 * i, b + REC_UNIT_DELTA_HEAD + 4 * k++, 4);
        } else {
            unsigned list = b[2], total, first, m;
            RecLossyList* l;
            if (len < 8 || (len - 8) % REC_ORDER_IMAGE || list > 1) return 0;
            total = get16(b + 4);
            first = get16(b + 6);
            m = (len - 8) / REC_ORDER_IMAGE;
            if (total > REC_ORDERS_MAX || first + m > total || (first && (e->pslot != s ||
                e->plist != list || e->ptotal != total || e->pnext != first)))
                return 0;
            l = &e->lists[s * 2 + list];
            if (!first) {
                if (total > l->cap) {
                    unsigned char* q = (unsigned char*)realloc(l->img, (size_t)total * REC_ORDER_IMAGE);
                    if (!q) return 0;
                    l->img = q;
                    l->cap = total;
                }
                l->n = total;
                e->pslot = s;
                e->plist = list;
                e->ptotal = total;
            }
            memcpy(l->img + (size_t)first * REC_ORDER_IMAGE, b + 8, (size_t)m * REC_ORDER_IMAGE);
            e->pnext = first + m;
        }
    }
    return 1;
}

/* The writer's stream state for one output file. */
typedef struct LossyOut {
    FILE* f;
    RecStream st;
    int on, exact;
    void* cctx;
    RecBytes stream, xs, pk;
    unsigned first, last, ticks, blocks, refreshes, sectioned;
    size_t written;
} LossyOut;

/* the stream block of the iterations [first, last], and its RB_EXACT */
static void lossy_flush(LossyOut* o, unsigned refresh)
{
    unsigned flags = RBF_STORED;
    const unsigned char* body = o->stream.p;
    size_t bn = o->stream.n, raw;
    if (!o->stream.n) return;
    if (o->on) {
        int k = reccodec_stream_block(&o->st, o->stream.p, o->stream.n, &flags, &body, &bn);
        o->sectioned += k == RECC_SECTIONS;
    } else if (reccodec_pack(o->cctx, &o->pk, o->stream.p, o->stream.n)) {
        flags = 0;
        body = o->pk.p;
        bn = o->pk.n;
    }
    raw = (flags & RBF_STORED) ? bn : (flags & RBF_SECTIONS) ? o->st.sec.n : o->stream.n;
    put_block(o->f, RB_STREAM, flags | refresh, o->first, o->last, (uint32_t)raw, body, bn);
    o->written += 32 + bn;
    o->blocks++;
    o->refreshes += refresh != 0;
    if (o->exact) {
        int packed = reccodec_pack(o->cctx, &o->pk, o->xs.p, o->xs.n);
        put_block(o->f, RB_EXACT, packed ? 0 : RBF_STORED, o->first, o->last, (uint32_t)o->xs.n,
                  packed ? o->pk.p : o->xs.p, packed ? o->pk.n : o->xs.n);
    }
    o->stream.n = o->xs.n = 0;
    o->ticks = 0;
}

/* IN's snapshots as (segment, GameTime), in file order, from its block heads alone: the filter
   is told a snapshot follows an iteration before IN's snapshot block comes, as the writer tells
   it before it takes one. NULL with *n 0 for none. */
typedef struct SnapAt { unsigned seg, tick; } SnapAt;

static SnapAt* snaps_of(const unsigned char* d, size_t n, unsigned* count)
{
    SnapAt* v = NULL;
    size_t at = 8;
    unsigned cap = 0, seg = 0;
    *count = 0;
    while (n - at >= 32) {
        RecBlock b;
        memcpy(&b, d + at, 32);
        if (b.magic != REC_BLOCK_MAGIC || b.packed > n - at - 32) break;
        seg += b.kind == RB_HEADER;
        if (b.kind == RB_SNAPSHOT) {
            if (*count == cap) {
                SnapAt* q = (SnapAt*)realloc(v, (cap = cap ? 2 * cap : 64) * sizeof *v);
                if (!q) break;
                v = q;
            }
            v[*count].seg = seg;
            v[(*count)++].tick = b.last;
        }
        at += 32 + b.packed;
    }
    return v;
}

static int cmd_lossy(const char* in, const char* outp, int exact)
{
    Reading g;
    LossyOut o;
    Exact e;
    RecLossy* L = NULL;
    RecBytes hb, one;
    unsigned char hold[REC_HOLD_BYTES];
    unsigned iters = 0, differ = 0, kept = 0, dropped = 0, snap_tick = 0, seg_iters = 0;
    unsigned last_tick = 0, last_refresh = 0, seg = 0, nsnaps, si = 0, snapped = 0;
    SnapAt* snaps;
    int k, bad = 0;
    memset(&o, 0, sizeof o);
    memset(&e, 0, sizeof e);
    memset(&hb, 0, sizeof hb);
    memset(&one, 0, sizeof one);
    if (!reading_open(&g, in)) return 2;
    snaps = snaps_of(g.r.d, g.r.n, &nsnaps);
    o.f = fopen(outp, "wb");
    o.cctx = ZSTD_createCCtx();
    if (!o.f || !o.cctx) return 2;
    o.exact = exact;
    fwrite(REC_MAGIC, 1, 8, o.f);
    hold_bytes(hold, 1);
    while (!bad && (k = reading_next(&g, &(double){ 0 })) > 0) {
        RecBlock* b = &g.r.b;
        if (b->kind == RB_HEADER) {
            RecLossyParams lp;
            lossy_flush(&o, 0);
            if (o.on) reccodec_stream_end(&o.st);
            o.on = g.p.usable && reccodec_stream_begin(&o.st, &g.p);
            reclossy_free(L);
            L = NULL;
            if (g.h.mode != 3) {
                lp.nslots = g.h.slots;
                lp.flags = REC_SKIRMISH_HOLDS;
                lp.hold_unit = REC_HOLD_UNIT;
                lp.hold_node = REC_HOLD_NODE;
                lp.gw = g.h.gw;
                lp.gh = g.h.gh;
                L = reclossy_begin(&lp);
                if (!L) {
                    printf("codectest: no memory for the filter of %u slots\n", g.h.slots);
                    bad = 2;
                    break;
                }
            }
            if (!exact_reset(&e, g.h.slots, g.h.gw, g.h.gh)) {
                bad = 2;
                break;
            }
            header_out(&hb, g.r.bytes, b->raw, L ? hold : NULL);
            put_block(o.f, RB_HEADER, RBF_STORED, 0, 0, (uint32_t)hb.n, hb.p, hb.n);
            seg_iters = 0;
            seg++;
        } else if (b->kind == RB_STREAM) {
            size_t at = 0, n = g.pay.n;
            while (at < n) {
                size_t end = at;
                unsigned tick;
                int refresh, snap, ok;
                RecLossyExact x;
                /* the iteration: its RK_TICK and the records up to the next */
                if (n - at < 8 || get16(g.pay.p + at) != RK_TICK || get16(g.pay.p + at + 2) != 4) {
                    printf("codectest: a stream block that does not begin an iteration at its "
                           "byte %lu\n", (unsigned long)at);
                    bad = 1;
                    break;
                }
                tick = get32(g.pay.p + at + 4);
                do {
                    end += ((size_t)4 + get16(g.pay.p + end + 2) + 3) & ~(size_t)3;
                } while (end + 4 <= n && get16(g.pay.p + end) != RK_TICK);
                if (end > n || (end < n && n - end < 4)) {
                    printf("codectest: a record cut short at GameTime %u\n", tick);
                    bad = 1;
                    break;
                }
                if (!exact_apply(&e, g.pay.p + at, end - at)) {
                    printf("codectest: an iteration the writer would refuse at GameTime %u\n", tick);
                    bad = 1;
                    break;
                }
                if (!seg_iters++) snap_tick = tick;
                refresh = reclossy_is_refresh(seg_iters, tick, last_refresh, REC_REFRESH_TICKS);
                /* a writer's snapshot follows a refresh, SNAPSHOT_TICKS or more after the last;
                   one is written where IN has one there */
                while (si < nsnaps && (snaps[si].seg < seg ||
                                       (snaps[si].seg == seg && snaps[si].tick < tick)))
                    si++;
                snap = refresh && tick - snap_tick >= LOSSY_SNAPSHOT_TICKS && si < nsnaps &&
                       snaps[si].seg == seg && snaps[si].tick == tick;
                if (snap) snapped = tick;
                if (!o.stream.n) o.first = tick;
                o.last = tick;
                x.units = e.units;
                x.live = e.live;
                x.lists = e.lists;
                x.grid = e.grid;
                one.n = 0;
                ok = L ? reclossy_iteration(L, g.pay.p + at, end - at, refresh, snap, &x, &one)
                       : (rb_put(&one, g.pay.p + at, end - at), !one.oom);
                if (!ok) {
                    printf("codectest: the filter refused the iteration at GameTime %u\n", tick);
                    bad = 1;
                    break;
                }
                differ += one.n != end - at || memcmp(one.p, g.pay.p + at, one.n);
                rb_put(&o.stream, one.p, one.n);
                if (exact) rb_put(&o.xs, g.pay.p + at, end - at);
                iters++;
                o.ticks++;
                last_tick = tick;
                if (refresh) {
                    if (L && !reclossy_equal(L, &x)) {
                        printf("codectest: the lossy world differs from the copy at the refresh at "
                               "GameTime %u\n", tick);
                        bad = 1;
                        break;
                    }
                    last_refresh = tick;
                    lossy_flush(&o, RBF_REFRESH);
                } else if (o.stream.n >= LOSSY_STREAM_BYTES || o.ticks >= LOSSY_STREAM_TICKS) {
                    lossy_flush(&o, 0);
                }
                at = end;
            }
        } else if (b->kind == RB_SNAPSHOT) {
            /* the snapshot the filter was told of */
            if (snapped && b->last == snapped && b->last == last_tick) {
                int packed = reccodec_pack(o.cctx, &o.pk, g.r.bytes, b->raw);
                put_block(o.f, RB_SNAPSHOT, packed ? 0 : RBF_STORED, b->first, b->last, b->raw,
                          packed ? o.pk.p : g.r.bytes, packed ? o.pk.n : b->raw);
                o.written += 32 + (packed ? o.pk.n : b->raw);
                snap_tick = b->last;
                snapped = 0;
                o.st.reset = 1;
                kept++;
            } else {
                dropped++;
            }
        } else if (b->kind == RB_END) {
            RecBytes eb;
            size_t at = 0;
            memset(&eb, 0, sizeof eb);
            lossy_flush(&o, 0);
            while (b->raw - at >= 4) {
                unsigned tag = get16(g.r.bytes + at), len = get16(g.r.bytes + at + 2);
                size_t step = (4 + (size_t)len + 3) & ~(size_t)3;
                if (len > b->raw - at - 4) break;
                if (tag != RT_CODEC_DECLINED && tag != RT_CODEC_MISMATCH) rb_put(&eb, g.r.bytes + at, step);
                at += step;
            }
            tlv_put(&eb, RT_CODEC_DECLINED, &o.st.declined, 4);
            tlv_put(&eb, RT_CODEC_MISMATCH, &o.st.mismatched, 4);
            put_block(o.f, RB_END, RBF_STORED, b->first, b->last, (uint32_t)eb.n, eb.p, eb.n);
            rb_free(&eb);
        } else if (b->kind == RB_EXACT) {
            /* the exact stream is the input's records, written again with the blocks above */
        } else {
            int packed = !(b->kind == RB_TIMELINES) && reccodec_pack(o.cctx, &o.pk, g.r.bytes, b->raw);
            put_block(o.f, b->kind, packed ? 0 : RBF_STORED, b->first, b->last, b->raw,
                      packed ? o.pk.p : g.r.bytes, packed ? o.pk.n : b->raw);
        }
    }
    if (!bad) lossy_flush(&o, 0);
    fclose(o.f);
    if (k < 0) {
        printf("codectest: %s: %s\n", in, g.r.why);
        bad = 2;
    }
    printf("lossy: %u iterations, %u whose records differ from the input's; %u stream blocks "
           "(%u refreshes, %u sectioned, %u declined, %u mismatched), %lu bytes of stream and "
           "snapshots; %u snapshots kept, %u dropped (not at a writer's refresh)\n", iters, differ,
           o.blocks, o.refreshes, o.sectioned, o.st.declined, o.st.mismatched,
           (unsigned long)o.written, kept, dropped);
    if (o.on) {
        if (!bad && (o.st.declined || o.st.mismatched)) bad = 1;
        reccodec_stream_end(&o.st);
    }
    reclossy_free(L);
    free(snaps);
    exact_free(&e);
    ZSTD_freeCCtx((ZSTD_CCtx*)o.cctx);
    rb_free(&o.stream);
    rb_free(&o.xs);
    rb_free(&o.pk);
    rb_free(&hb);
    rb_free(&one);
    reading_close(&g);
    return bad;
}

#ifdef _WIN32
/* ---- the DLL's reader against the exact stream (sprays) ----------------------------------------- */

/* The reader logs through the DLL's log sink, which this test has none of. */
void tagpu_logf(const char* fmt, ...)
{
    (void)fmt;
}

/* The first segment's exact iterations (RB_EXACT): each one's GameTime and its sprays, 48 bytes
   each in the order the capture made them, and the segment's snapshots. */
typedef struct ExIter { uint32_t tick; size_t at; unsigned n; } ExIter;
typedef struct ExactSprays {
    ExIter*  it;
    unsigned nit, itcap;
    RecBytes body;
    unsigned snap[4096], nsnap;
} ExactSprays;

static int exact_sprays(Reading* g, ExactSprays* x)
{
    unsigned segs = 0;
    int k;
    while ((k = next_block(&g->r)) > 0) {
        const RecBlock* b = &g->r.b;
        size_t at = 0;
        if (b->kind == RB_HEADER && ++segs > 1) break;
        if (b->kind == RB_SNAPSHOT && x->nsnap < sizeof x->snap / sizeof *x->snap)
            x->snap[x->nsnap++] = b->last;
        if (b->kind != RB_EXACT) continue;
        while (b->raw - at >= 4) {
            unsigned kind = get16(g->r.bytes + at), len = get16(g->r.bytes + at + 2);
            const unsigned char* body = g->r.bytes + at + 4;
            if (len > b->raw - at - 4) break;
            if (kind == RK_TICK && len == 4) {
                uint32_t t = get32(body);
                if (x->nit && t <= x->it[x->nit - 1].tick) {
                    snprintf(g->r.why, sizeof g->r.why, "an exact iteration of GameTime %u after %u",
                             t, x->it[x->nit - 1].tick);
                    return -1;
                }
                if (x->nit == x->itcap) {
                    ExIter* q = (ExIter*)realloc(x->it, (x->itcap ? x->itcap * 2 : 4096) * sizeof *q);
                    if (!q) return -1;
                    x->it = q;
                    x->itcap = x->itcap ? x->itcap * 2 : 4096;
                }
                x->it[x->nit].tick = t;
                x->it[x->nit].at = x->body.n;
                x->it[x->nit++].n = 0;
            } else if (kind == RK_EV_SPRAY && len == 4 + REC_SPRAY_BODY && x->nit) {
                rb_put(&x->body, body, len);
                x->it[x->nit - 1].n++;
            }
            at += (4 + (size_t)len + 3) & ~(size_t)3;
        }
    }
    return k < 0 || x->body.oom ? -1 : 1;
}

static const ExIter* exact_at(const ExactSprays* x, uint32_t t)
{
    unsigned lo = 0, hi = x->nit;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (x->it[mid].tick < t) lo = mid + 1;
        else hi = mid;
    }
    return lo < x->nit && x->it[lo].tick == t ? &x->it[lo] : NULL;
}

/* a spray and its place in its iteration, to order by unit ID and then by that place */
typedef struct SprayRef { uint32_t id; unsigned i; const unsigned char* b; } SprayRef;

static int spray_ref_order(const void* a, const void* b)
{
    const SprayRef *x = (const SprayRef*)a, *y = (const SprayRef*)b;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return (x->i > y->i) - (x->i < y->i);
}

/* The reader's sprays of the iteration its world holds against the exact ones: equal when, by
   unit ID, each unit's sprays are the same 48 bytes in the same order -- the multiset of every
   spray's bytes, and each unit's order. 1 equal, 0 not (printed), -1 no memory. */
static int sprays_equal(const RecWorld* w, const ExactSprays* x, unsigned long* count)
{
    const ExIter* e = exact_at(x, w->tick);
    SprayRef *a, *b;
    unsigned n = 0, i, m = e ? e->n : 0;
    int ok = 1;
    for (i = 0; i < w->nev; i++) n += w->ev[i].kind == RK_EV_SPRAY;
    if (!e) {
        printf("sprays: the reader read GameTime %u, which the exact stream has no iteration of\n",
               w->tick);
        return 0;
    }
    a = (SprayRef*)malloc((n + 1) * sizeof *a);
    b = (SprayRef*)malloc((m + 1) * sizeof *b);
    if (!a || !b) {
        free(a);
        free(b);
        return -1;
    }
    for (n = 0, i = 0; i < w->nev; i++) {
        if (w->ev[i].kind != RK_EV_SPRAY) continue;
        if (w->ev[i].len != 4 + REC_SPRAY_BODY) {
            printf("sprays: GameTime %u: a spray of %u bytes\n", w->tick, w->ev[i].len);
            ok = 0;
            break;
        }
        a[n].id = get32(w->ev[i].b);
        a[n].i = n;
        a[n].b = w->ev[i].b;
        n++;
    }
    for (i = 0; i < m; i++) {
        b[i].b = x->body.p + e->at + (size_t)i * (4 + REC_SPRAY_BODY);
        b[i].id = get32(b[i].b);
        b[i].i = i;
    }
    if (ok && n != m) {
        printf("sprays: GameTime %u: the reader has %u sprays, the exact stream %u\n", w->tick, n, m);
        ok = 0;
    }
    if (ok) {
        qsort(a, n, sizeof *a, spray_ref_order);
        qsort(b, m, sizeof *b, spray_ref_order);
        for (i = 0; i < n && ok; i++)
            if (memcmp(a[i].b, b[i].b, 4 + REC_SPRAY_BODY)) {
                printf("sprays: GameTime %u: unit ID %u's sprays differ from the exact stream's\n",
                       w->tick, a[i].id);
                ok = 0;
            }
    }
    *count += n;
    free(a);
    free(b);
    return ok;
}

/* codectest sprays IN: the DLL's own reader (tagpu_recreader.c) over IN's first timeline, its
   sprays at every iteration against the exact stream's (RB_EXACT, the test lever
   tagpu_rec.exact): read in order from the start, and after a seek to each snapshot's GameTime,
   to the one after it -- read on to the next snapshot -- and to one 151 GameTimes after it. */
static int cmd_sprays(const char* in)
{
    Reading g;
    ExactSprays x;
    RecReader* r;
    char why[512];
    unsigned long seq = 0, sprays = 0, seq_sprays, after = 0, seeks = 0;
    unsigned i, last;
    int k, bad = 0;
    memset(&x, 0, sizeof x);
    if (!reading_open(&g, in)) return 2;
    if (exact_sprays(&g, &x) < 0) {
        printf("codectest: %s: %s\n", in, g.r.why);
        reading_close(&g);
        return 2;
    }
    reading_close(&g);
    if (!x.nit) {
        printf("codectest: %s: no exact stream (RB_EXACT): record with tagpu_rec.exact\n", in);
        free(x.it);
        rb_free(&x.body);
        return 2;
    }
    last = x.it[x.nit - 1].tick;
    r = recr_open(in, why, sizeof why);
    if (!r) {
        printf("codectest: %s: %s\n", in, why);
        free(x.it);
        rb_free(&x.body);
        return 2;
    }
    while (!bad && (k = recr_tick(r, why, sizeof why)) == 1) {
        int e = sprays_equal(recr_world(r), &x, &sprays);
        if (e <= 0) bad = e < 0 ? 2 : 1;
        seq++;
    }
    if (!bad && k < 0) {
        printf("sprays: the reader refused the stream in order: %s\n", why);
        bad = 1;
    }
    seq_sprays = sprays;
    if (!bad && seq != x.nit) {
        printf("sprays: the reader read %lu iterations in order, the exact stream holds %u\n", seq,
               x.nit);
        bad = 1;
    }
    for (i = 0; !bad && i < x.nsnap; i++) {
        unsigned s = x.snap[i], end = i + 1 < x.nsnap ? x.snap[i + 1] : last + 1, d;
        static const unsigned delta[3] = { 0, 1, 151 };
        for (d = 0; !bad && d < 3; d++) {
            unsigned t = s + delta[d];
            int e;
            if (t > last) break;
            if (recr_seek(r, t, 0, NULL, NULL, why, sizeof why) != 1) {
                printf("sprays: the seek to GameTime %u: %s\n", t, why);
                bad = 1;
                break;
            }
            if (delta[d] && (!recr_seek_work(r)->from_snapshot || recr_seek_work(r)->from != s)) {
                printf("sprays: the seek to GameTime %u built from %u, not from the snapshot at %u\n",
                       t, recr_seek_work(r)->from, s);
                bad = 1;
                break;
            }
            seeks++;
            e = sprays_equal(recr_world(r), &x, &sprays);
            if (e <= 0) bad = e < 0 ? 2 : 1;
            after++;
            /* from the snapshot's next iteration, on to the next snapshot */
            while (!bad && delta[d] == 1 && t + 1 < end && (k = recr_tick(r, why, sizeof why)) == 1) {
                t = recr_world(r)->tick;
                e = sprays_equal(recr_world(r), &x, &sprays);
                if (e <= 0) bad = e < 0 ? 2 : 1;
                after++;
            }
            if (!bad && delta[d] == 1 && t + 1 < end) {
                printf("sprays: reading on from the snapshot at %u stopped at GameTime %u: %s\n", s,
                       t, k < 0 ? why : "the end");
                bad = 1;
            }
        }
    }
    printf("sprays: %s: timeline 0, %lu iterations read in order, %lu sprays; %lu seeks from %u "
           "snapshots and %lu iterations compared after them, %lu sprays%s\n", in, seq, seq_sprays,
           seeks, x.nsnap, after, sprays - seq_sprays,
           bad ? "" : ": every iteration's sprays the exact stream's, as a multiset and each unit's "
                      "in order");
    recr_close(r);
    free(x.it);
    rb_free(&x.body);
    return bad;
}
#endif

/* ---- outcomes --------------------------------------------------------------------------------- */

static void rec_put(RecBytes* b, unsigned kind, const void* body, unsigned len)
{
    unsigned char h[4] = { (unsigned char)kind, (unsigned char)(kind >> 8), (unsigned char)len,
                           (unsigned char)(len >> 8) };
    static const unsigned char zero[4];
    rb_put(b, h, 4);
    rb_put(b, body, len);
    rb_put(b, zero, ((4 + len + 3) & ~3u) - 4 - len);
}

static void rec_unit(RecBytes* b, unsigned s, const uint32_t* img)
{
    unsigned char body[4 + IMG];
    memset(body, 0, 4);
    body[0] = (unsigned char)s;
    body[1] = (unsigned char)(s >> 8);
    memcpy(body + 4, img, IMG);
    rec_put(b, RK_UNIT, body, sizeof body);
}

/* a delta of the dwords that differ, and of `force` (an unchanged dword) as well */
static void rec_delta(RecBytes* b, unsigned s, const uint32_t* old, const uint32_t* img, int force)
{
    unsigned char body[REC_UNIT_DELTA_HEAD + 4 * NDW];
    unsigned i, nb = 0;
    memset(body, 0, sizeof body);
    body[0] = (unsigned char)s;
    body[1] = (unsigned char)(s >> 8);
    for (i = 0; i < NDW; i++)
        if (old[i] != img[i] || (int)i == force) {
            body[2 + (i >> 3)] |= (unsigned char)(1u << (i & 7));
            memcpy(body + REC_UNIT_DELTA_HEAD + 4 * nb++, &img[i], 4);
        }
    if (nb) rec_put(b, RK_UNIT_DELTA, body, REC_UNIT_DELTA_HEAD + 4 * nb);
}

/* the lane of `role` in the initial table, read or added to */
static void lane_add(uint32_t* img, unsigned role, uint32_t add, int set)
{
    unsigned j;
    for (j = 0; j < REC_LANES_N; j++)
        if (rec_lanes[j].role == role)
            lane_put((unsigned char*)img, &rec_lanes[j], (set ? 0 : lane_get((unsigned char*)img, &rec_lanes[j])) + add);
}

static uint32_t lane_val(const uint32_t* img, unsigned role)
{
    unsigned j;
    for (j = 0; j < REC_LANES_N; j++)
        if (rec_lanes[j].role == role) return lane_get((const unsigned char*)img, &rec_lanes[j]);
    return 0;
}

/* a builder's ledger for one iteration: the requested grows by 1.5 energy and 0.25 metal a tick
   from 0 after its owner's settle, every 30 ticks; the granted follows it, or for a refused
   builder stays where it was */
static void ledger_tick(uint32_t* img, uint32_t gt, int refused)
{
    static const unsigned req[2] = { REC_LR_EREQ, REC_LR_MREQ }, grant[2] = { REC_LR_EGRANT, REC_LR_MGRANT };
    static const uint32_t step[2] = { 0x3FC00000u, 0x3E800000u };
    unsigned k;
    for (k = 0; k < 2; k++) {
        uint32_t r = gt % 30 == 1 ? 0 : reccodec_f32_add(lane_val(img, req[k]), step[k]);
        lane_add(img, req[k], r, 1);
        lane_add(img, grant[k], !r ? 0 : refused ? lane_val(img, grant[k]) : r, 1);
    }
}

/* ---- the outcomes' projectile pool: an engine in miniature and its capture ---------------------- */

#define PJ_N 64                         /* RT_PROJECTILES of the outcomes' header */
enum { PJ_NONE, PJ_TWICE, PJ_HEADS, PJ_SHRINK, PJ_UNCHANGED, PJ_PAD };

typedef struct PSim {
    uint32_t eng[PJ_N][PDW];            /* the engine's pool, every slot's bytes            */
    uint32_t sh[PJ_N][PDW];             /* the capture's last image of each slot            */
    unsigned count, hw, scount, shw, first;
    unsigned char shot[4][SHOT];        /* fired in the step after the iteration: its births */
    unsigned nshot;
    unsigned births, deaths, guided, tail, grows;   /* what the iterations exercised       */
} PSim;

/* The step: every record moves (a guided one turns, speeds up and takes the guided rule's
   velocity, the rest fall), the last step's shots are appended at the count over what their
   slots held, and the compaction stamps each record with its index and drops the spent ones,
   leaving the slots past the new count as they were -- the engine's pool as
   exe-reverse-engineering.md has it. */
static void psim_step(PSim* q)
{
    unsigned i, k, w;
    for (i = 0; i < q->count; i++) {
        uint32_t* r = q->eng[i];
        if (r[0] == 1) {
            uint32_t v[3], sp = (r[14] >> 16 | r[15] << 16) + 0x2000;
            if (sp > 0x60000) sp = 0x60000;
            r[13] += 0x300u << 16;
            r[14] = (r[14] & 0xFFFF) | sp << 16;
            r[15] = (r[15] & 0xFFFF0000u) | sp >> 16;
            guided(r, v);
            r[7] = v[0];
            r[8] = v[1];
            r[9] = v[2];
            q->guided++;
        } else {
            r[8] -= 0x800;
        }
        r[1] += r[7];
        r[2] += r[8];
        r[3] += r[9];
        r[22]--;
    }
    for (k = 0; k < q->nshot && q->count < PJ_N; k++) {
        const unsigned char* b = q->shot[k];
        unsigned char* r = (unsigned char*)q->eng[q->count++];
        put32(r, b[0x19] + 1u);
        memcpy(r + 0x04, b + 0x01, 12);
        memcpy(r + 0x10, b + 0x01, 12);
        put32(r + 0x1C, 0x9000u * (k + 1));
        put32(r + 0x20, 0x4000);
        put32(r + 0x24, 0xFFFF7000u);
        memcpy(r + 0x28, b + 0x0D, 12);
        put32(r + 0x34, 0x00400000u * (k + 2));
        put32(r + 0x38, 0x00011000u + 0x300 * k);
        put32(r + 0x4E, get16(b + 0x1F));
        put32(r + 0x52, get16(b + 0x21));
        put32(r + 0x58, 6 + (q->births + k) % 4);
        q->births++;
    }
    for (i = 0, w = 0; i < q->count; i++) {
        uint32_t* r = q->eng[i];
        r[25] = (r[25] & 0x00FFFFFFu) | (i & 0xFF) << 24;
        r[26] = (r[26] & 0xFFFFFF00u) | (i >> 8 & 0xFF);
        if ((int32_t)r[22] <= 0) {
            q->deaths++;
            continue;
        }
        if (w != i) memcpy(q->eng[w], r, PIMG);
        w++;
    }
    q->count = w;
    if (q->count > q->hw) {
        q->hw = q->count;
        q->grows++;
    }
}

/* The capture (cap_proj): a head when the count or the extent changed, then every slot below the
   extent whose image changed, whole when it is new to the extent or when a delta is no smaller.
   `fault` makes the iteration one the codec must decline: a correction naming a slot again, a
   second head, an extent that shrinks (giving up the engine's stale slots with it), a delta
   naming an unchanged dword, a delta whose pad is not zero. */
static void psim_capture(PSim* q, RecBytes* b, int fault)
{
    unsigned i, d, n, again = ~0u, deltas = 0;
    uint32_t hd[2];
    int head = q->first || q->count != q->scount || q->hw != q->shw;
    if (fault == PJ_SHRINK && q->count < q->hw) {
        memset(q->eng[q->count], 0, (size_t)(q->hw - q->count) * PIMG);
        memset(q->sh[q->count], 0, (size_t)(q->shw - q->count) * PIMG);
        q->hw = q->shw = q->count;
        head = 1;
    }
    hd[0] = q->count;
    hd[1] = q->hw;
    if (head || fault == PJ_HEADS) rec_put(b, RK_PROJ_HEAD, hd, 8);
    if (fault == PJ_HEADS) rec_put(b, RK_PROJ_HEAD, hd, 8);
    for (i = 0; i < q->hw; i++) {
        uint32_t* r = q->eng[i], * sh = q->sh[i];
        unsigned char body[4 + PIMG];
        int fresh = i >= q->shw;
        for (d = 0, n = 0; d < PDW; d++) n += r[d] != sh[d];
        if (!fresh && !n) continue;
        if (fresh || REC_PROJ_DELTA_HEAD + 4 * n >= 4 + PIMG) {
            rec_put(b, RK_PROJ, body, pool_body(RK_PROJ, i, sh, r, body));
        } else {
            unsigned len = pool_body(RK_PROJ_DELTA, i, sh, r, body);
            if (fault == PJ_UNCHANGED && !deltas) {
                uint32_t bits = get32(body + 2);
                for (d = 0; d < PDW && r[d] != sh[d]; d++) ;
                put32(body + 2, bits | (uint32_t)1 << d);
                len = 8;
                for (n = 0; n < PDW; n++)
                    if (bits >> n & 1 || n == d) {
                        put32(body + len, r[n]);
                        len += 4;
                    }
            }
            if (fault == PJ_PAD && !deltas) body[7] = 1;
            rec_put(b, RK_PROJ_DELTA, body, len);
            deltas++;
        }
        if (i >= q->count) q->tail++;
        if (again == ~0u) again = i;
        memcpy(sh, r, PIMG);
    }
    if (fault == PJ_TWICE && again != ~0u) {
        unsigned char body[4 + PIMG];
        uint32_t c = 0;
        rec_put(b, RK_CORRECT, &c, 4);
        rec_put(b, RK_PROJ, body, pool_body(RK_PROJ, again, q->sh[again], q->eng[again], body));
    }
    q->scount = q->count;
    q->shw = q->hw;
    q->first = 0;
}

/* the shots of the step after the iteration, as RK_EV_SHOT: two shooters, a guided weapon (ID 1)
   every other step and a ballistic one (ID 2) every step */
static void psim_shots(PSim* q, RecBytes* b, uint32_t gt)
{
    unsigned k;
    q->nshot = 0;
    for (k = 0; k < 2; k++) {
        unsigned char* s = q->shot[q->nshot];
        if (k == 0 && gt % 2) continue;
        memset(s, 0, SHOT);
        s[0] = 0x0D;
        put32(s + 0x01, 0x01000000u * (3 + k) + gt * 0x1000);
        put32(s + 0x05, 0x00200000u);
        put32(s + 0x09, 0x02000000u * (1 + k));
        put32(s + 0x0D, 0x05000000u);
        put32(s + 0x11, 0x00100000u);
        put32(s + 0x15, 0x06000000u + gt * 0x10);
        s[0x19] = (unsigned char)k;
        s[0x1F] = 9;
        s[0x21] = (unsigned char)(3 + k);
        rec_put(b, RK_EV_SHOT, s, SHOT);
        q->nshot++;
    }
}

/* Six iterations of five units: two ground movers and an aircraft moving, a still unit, and one
   whose plain dword changes unpredictably; the first and the fourth building, the fourth refused
   from tick 40 on (ledger_tick); and the projectile pool, `pfault` in the fourth
   iteration. `force`: in the fourth iteration a delta names a dword of the still unit it does not
   change; `correct`: a correction of a unit whose delta the same iteration carries. */
static void synth_block(RecBytes* b, uint32_t units[][NDW], PSim* q, uint32_t* gt, int create,
                        int force, int correct, int pfault)
{
    static uint32_t prev[6][NDW];
    unsigned it, s;
    b->n = 0;
    for (it = 0; it < 6; it++, (*gt)++) {
        uint32_t c = 0;
        rec_put(b, RK_TICK, gt, 4);
        psim_step(q);
        psim_capture(q, b, it == 3 ? pfault : PJ_NONE);
        psim_shots(q, b, *gt);
        if (create && !it) {
            for (s = 1; s <= 5; s++) {
                memset(units[s], 0, IMG);
                units[s][0] = 0x1000 + s;
                lane_add(units[s], REC_LR_TYPE, s == 3 ? 51 : 10 + s, 1);
                lane_add(units[s], REC_LR_X, 0x100000u * s, 1);
                lane_add(units[s], REC_LR_Z, 0x200000u * s, 1);
                lane_add(units[s], REC_LR_LAYER, s == 3 ? 2 : 1, 1);
                lane_add(units[s], REC_LR_FW, 2, 1);
                lane_add(units[s], REC_LR_FH, 2, 1);
                lane_add(units[s], REC_LR_OWNER, 1 + s % 2, 1);
                rec_unit(b, s, units[s]);
            }
            continue;
        }
        memcpy(prev, units, sizeof prev);
        for (s = 1; s <= 3; s++) {
            lane_add(units[s], REC_LR_X, 0x8000u * s, 0);
            lane_add(units[s], REC_LR_VX, 0x8000u * s, 1);
            lane_add(units[s], REC_LR_STAMP, *gt - 1, 1);
            lane_add(units[s], REC_LR_HEADING, 37 * s, 0);
        }
        units[5][NDW - 10] ^= *gt * 2654435761u;
        ledger_tick(units[1], *gt, 0);
        ledger_tick(units[4], *gt, *gt >= 40);
        for (s = 1; s <= 5; s++) rec_delta(b, s, prev[s], units[s], force && it == 3 && s == 4 ? 7 : -1);
        if (correct && it == 3) {
            rec_put(b, RK_CORRECT, &c, 4);
            units[2][9] ^= 0x55;
            rec_unit(b, 2, units[2]);
        }
    }
}

static int expect(const char* what, int got, int want)
{
    printf("%-60s %s\n", what, got == want ? "ok" : "FAILED");
    return got == want;
}

/* decode a written block as a reader would: from the world at a reset, else continuing */
static int reader_check(RecCodec* r, World* w, const RecCodecParams* p, unsigned flags,
                        const unsigned char* body, size_t bn, size_t raw, const unsigned char* pay, size_t n,
                        void* dctx)
{
    RecBytes out, un;
    char why[256];
    int ok;
    memset(&out, 0, sizeof out);
    memset(&un, 0, sizeof un);
    if (!(flags & RBF_STORED)) {
        rb_grow(&un, raw);
        if (!reccodec_unpack(dctx, un.p, raw, body, bn)) return 0;
        body = un.p;
        bn = raw;
    }
    if (flags & RBF_SECTIONS) {
        if (flags & RBF_FRAME_RESET) {
            RecFrameWorld v = world_view(w);
            reccodec_restart(r, p, w->unit, w->live, w->pool, w->pcount, w->phw, &v);
        }
        ok = reccodec_decode(r, body, bn, &out, why, sizeof why) && out.n == n && !memcmp(out.p, pay, n);
        if (!ok) printf("  reader: %s\n", why);
    } else {
        reccodec_invalidate(r);
        ok = bn == n && !memcmp(body, pay, n);
    }
    world_apply(w, pay, n);
    rb_free(&out);
    rb_free(&un);
    return ok;
}

/* A frame of `n` bytes in one raw block whose header asks for a window of 2^wlog (no single
   segment, a 4-byte content size): the window is all that differs from a frame zstd writes. */
static size_t frame_with_window(unsigned char* f, unsigned wlog, const unsigned char* src, size_t n)
{
    f[0] = 0x28; f[1] = 0xB5; f[2] = 0x2F; f[3] = 0xFD;
    f[4] = 0x80;                                  /* a 4-byte content size, no checksum   */
    f[5] = (unsigned char)((wlog - 10) << 3);     /* the exponent, no mantissa            */
    put32(f + 6, (uint32_t)n);
    f[10] = (unsigned char)(1 | n << 3);          /* the last block, raw, n bytes         */
    f[11] = (unsigned char)(n >> 5);
    f[12] = (unsigned char)(n >> 13);
    memcpy(f + 13, src, n);
    return 13 + n;
}

/* reccodec_unpack refuses a window past 2^27 as the Python reader does, and nothing else of
   such a frame: the same bytes at 2^27 unpack. */
static int zstd_window_check(void* cctx, void* dctx)
{
    static const unsigned char src[64] = "a block's bytes, which only the frame's window distinguishes";
    static unsigned char zero[4096], back[sizeof zero];
    unsigned char f[13 + sizeof src], out[sizeof src];
    RecBytes pk;
    size_t fn;
    int ok = 1;
    memset(&pk, 0, sizeof pk);
    fn = frame_with_window(f, 27, src, sizeof src);
    ok &= expect("zstd: a frame asking for a 2^27 window unpacks",
                 reccodec_unpack(dctx, out, sizeof out, f, fn) && !memcmp(out, src, sizeof src), 1);
    fn = frame_with_window(f, 28, src, sizeof src);
    ok &= expect("zstd: the same frame asking for 2^28 is refused",
                 reccodec_unpack(dctx, out, sizeof out, f, fn), 0);
    ok &= expect("zstd: the writer's own frame unpacks",
                 reccodec_pack(cctx, &pk, zero, sizeof zero) &&
                 reccodec_unpack(dctx, back, sizeof back, pk.p, pk.n), 1);
    rb_free(&pk);
    return ok;
}

/* ---- each record frame's own outcomes ------------------------------------------------------------

   Each takes the segment's params of cmd_outcomes and returns 1 when every check of its holds:
   blocks the frame codes and reads back, each record it cannot carry latched and its block
   DECLINED with the next one restarting, and the decoder's own refusals. */

/* section `s` of a sectioned container: its bounds */
static void sec_bounds(const unsigned char* c, unsigned s, size_t* a, size_t* e)
{
    unsigned i;
    *a = sizeof(RecSecHead);
    for (i = 0; i < s; i++) *a += get32(c + 8 + 4 * i);
    *e = *a + get32(c + 8 + 4 * s);
}

/* a record frame's section: its column table at *tab and column i at [col[i], col[i + 1]); the
   column count, or 0 when the table does not read (defined with the fuzz) */
static unsigned frame_regions(const unsigned char* sec, size_t a, size_t e, size_t* tab,
                              size_t col[RECF_COLS_MAX + 1]);

enum { CF_NONE, CF_PAD, CF_ROW, CF_Z, CF_EMPTY, CF_LEN };
#define CF_W    16                      /* cmd_outcomes' grid: 16 x 16                           */
#define CF_HELD (4 * REC_CELLS_MAX)     /* the frame's CELLS_HELD: T's cells at most              */

/* an RK_CELLS of the run [x, x + n) of row z of `grid`, then made malformed by `fault` */
static void cells_rec(RecBytes* b, const unsigned char* grid, unsigned x, unsigned z, unsigned n,
                      int fault)
{
    unsigned char body[8 + REC_CELLS_MAX * REC_CELL_BYTES];
    unsigned len = 8 + n * REC_CELL_BYTES;
    memset(body, 0, 8);
    memcpy(body + 8, grid + ((size_t)z * CF_W + x) * REC_CELL_BYTES, (size_t)n * REC_CELL_BYTES);
    if (fault == CF_PAD) body[6] = 1;
    if (fault == CF_ROW) x = CF_W - n + 1;
    if (fault == CF_Z) z = CF_W;
    if (fault == CF_EMPTY) n = 0, len = 8;
    if (fault == CF_LEN) len--;
    body[0] = (unsigned char)x;
    body[2] = (unsigned char)z;
    body[4] = (unsigned char)n;
    body[5] = (unsigned char)(n >> 8);
    rec_put(b, RK_CELLS, body, len);
}

/* the feature tick's bracket, its RK_EV_FEATURE or its RK_EV_FEATURE_END */
static void cells_bracket(RecBytes* b, int end)
{
    unsigned char e[8] = { FR_TICK, REC_PHASE_LATE, 0, 0, 0x80, 0x55, 0x49, 0 };
    if (end) memset(e + 1, 0, 7);
    rec_put(b, end ? RK_EV_FEATURE_END : RK_EV_FEATURE, e, 8);
}

/* A sectioned container's cells frame as this build writes it: its flag column into flag[] (at
   most 64), and whether a sym says slot A emptied. 0: it is not. */
static int cells_flags(const RecBytes* sec, unsigned char* flag, unsigned* nflag, int* emptied)
{
    RecSecHead h;
    const unsigned char* p;
    size_t at = 1, len[9], n, i, off = sizeof h;
    uint32_t v;
    if (sec->n < sizeof h) return 0;
    memcpy(&h, sec->p, sizeof h);
    for (i = 0; i < RCS_CELLS; i++) off += h.len[i];
    p = sec->p + off;
    n = h.len[RCS_CELLS];
    if (off + n > sec->n || n < 1 || p[0] != 2 || !varint_get(p, n, &at, &v) || v != 9) return 0;
    for (i = 0; i < 9; i++) {
        if (!varint_get(p, n, &at, &v)) return 0;
        len[i] = v;
    }
    if (len[0] > 64 || at + len[0] + len[1] + len[2] > n) return 0;
    memcpy(flag, p + at, len[0]);
    *nflag = (unsigned)len[0];
    at += len[0] + len[1];
    *emptied = 0;
    for (i = 0; i < len[2]; i++) *emptied |= p[at + i] == 0x40;
    return 1;
}

/* A segment of the 16 x 16 grid. Its first iteration's images are every cell; each later one's
   repeat the last iteration's tick input -- the flag carries them -- or add to it, and are coded;
   each tick input changes three runs, in one of which a unit leaves slot A and comes back. Blocks
   0-2 SECTIONS and read back; then each malformed RK_CELLS DECLINED, followed by a restarting
   SECTIONS; then a tick input past CELLS_HELD, which T does not keep, so the next images are
   coded: SECTIONS; then a block that declines in its fourth iteration, whose images from there on
   reach the encoder's world through plain_apply alone, and a restarting SECTIONS. After every
   block the encoder's grid is the reader's: through the latch of the malformed records and
   through both declines. Then the restart from the reader's grid: a block restarting as after a
   snapshot whose one iteration re-images a run the reader holds, which codes as no change; the
   same block with the check decoder restarted from a grid one cell off, MISMATCH, and a
   restarting SECTIONS; and a reader's restart from a world of another grid, or none, refused. */
static int outcomes_cells(const RecCodecParams* p)
{
    static unsigned char grid[CF_W * CF_W * REC_CELL_BYTES];
    static const struct { unsigned x, z, n; } run[3] = { { 2, 3, 5 }, { 0, 9, 16 }, { 11, 14, 4 } };
    static uint32_t unit[NDW];
    /* the cells frame's columns (tagpu_recframe_cells.c, THE BYTES) */
    enum { CC_FLAG, CC_REC, CC_SYM, CC_F0, CC_N = CC_F0 + 6 };
    static const char* what[18] = {
        "SECTIONS, the first iteration's every cell", "SECTIONS, images flagged and coded",
        "SECTIONS, continuing", "DECLINED: an RK_CELLS whose pad is not zero",
        "SECTIONS, restarting after it", "DECLINED: an RK_CELLS past its row's end",
        "SECTIONS, restarting after it", "DECLINED: an RK_CELLS past the grid's rows",
        "SECTIONS, restarting after it", "DECLINED: an RK_CELLS of no cells",
        "SECTIONS, restarting after it", "DECLINED: an RK_CELLS one byte short",
        "SECTIONS, restarting: images after a tick input past CELLS_HELD",
        "DECLINED in mid-block: a delta naming an unchanged dword",
        "SECTIONS, restarting after it",
        "SECTIONS, restarting from the reader's grid: the cells it already holds code as no change",
        "MISMATCH: the check decoder restarted from another grid", "SECTIONS, restarting after it" };
    RecStream st;
    RecCodec* rd;
    World w;
    RecBytes y, keep, out;
    RecFrameWorld fw;
    uint32_t gt = 1, seed = 12345;
    unsigned block, it, i, k, c, flags, nflag = 0, ok = 1;
    unsigned char flag[64];
    int emptied = 0, o;
    const unsigned char* body;
    size_t bn, a, e, tab, col[RECF_COLS_MAX + 1];
    char line[128], why[256];
    memset(&w, 0, sizeof w);
    memset(&y, 0, sizeof y);
    memset(&keep, 0, sizeof keep);
    memset(&out, 0, sizeof out);
    for (i = 0; i < sizeof grid; i++) grid[i] = (unsigned char)((seed = seed * 1103515245u + 12345u) >> 24);
    if (!reccodec_stream_begin(&st, p) || !(rd = reccodec_new(p))) return 0;
    world_reset(&w, p->nslots, p->nproj, p->gw, p->gh);
    unit[0] = 0x1001;
    lane_add(unit, REC_LR_TYPE, 11, 1);
    lane_add(unit, REC_LR_X, 0x100000u, 1);
    lane_add(unit, REC_LR_Z, 0x200000u, 1);
    lane_add(unit, REC_LR_LAYER, 1, 1);
    lane_add(unit, REC_LR_FW, 2, 1);
    lane_add(unit, REC_LR_FH, 2, 1);
    for (block = 0; block < 18; block++) {
        int fault = block >= 3 && block <= 11 && block % 2 ? (int)(block - 1) / 2 : CF_NONE;
        if (block == 15) st.reset = 1;           /* as a snapshot does */
        if (block == 16) {
            /* E from its own world, D from that grid with one byte of run[0]'s first cell off;
               the block's image of run[0] codes as no change, so D rebuilds the cell from its
               own grid and gives back other records */
            static unsigned char other[CF_W * CF_W * REC_CELL_BYTES];
            unsigned count, hw;
            const unsigned char* pool = reccodec_pool(st.enc, &count, &hw);
            RecFrameWorld ow;
            reccodec_world(st.enc, &fw);
            memcpy(other, fw.grid, sizeof other);
            other[((size_t)run[0].z * CF_W + run[0].x) * REC_CELL_BYTES + 5] ^= 0x20;
            ow = fw;
            ow.grid = other;
            reccodec_restart(st.enc, &st.p, reccodec_units(st.enc), reccodec_live(st.enc), pool, count, hw, &fw);
            reccodec_restart(st.chk, &st.p, reccodec_units(st.enc), reccodec_live(st.enc), pool, count, hw, &ow);
            st.reset = 0;
        }
        y.n = 0;
        for (it = 0; it < (block == 12 ? 2u : block == 15 || block == 16 ? 1u : fault ? 5u : 6u); it++, gt++) {
            rec_put(&y, RK_TICK, &gt, 4);
            if (block == 15 || block == 16) {
                /* run[0] as the reader's grid holds it, and no tick input */
                cells_rec(&y, w.grid, run[0].x, run[0].z, run[0].n, CF_NONE);
                continue;
            }
            /* block 13: a unit created in its first iteration, and in its fourth a delta naming a
               dword that does not change, which the unit frame cannot carry (cmd_outcomes' block
               2): the block declines there, and its last three iterations go through plain_apply */
            if (block == 13 && it == 0) rec_unit(&y, 1, unit);
            if (block == 13 && it == 3) rec_delta(&y, 1, unit, unit, 7);
            if (block == 12) {
                /* 1,025 runs of a row: 16,400 cells of tick input, then the same as images */
                if (!it) cells_bracket(&y, 0);
                for (k = 0; k <= CF_HELD / CF_W; k++) cells_rec(&y, grid, 0, 0, CF_W, CF_NONE);
                if (!it) cells_bracket(&y, 1);
                continue;
            }
            if (block == 0 && !it) {
                for (k = 0; k < CF_W; k++) cells_rec(&y, grid, 0, k, CF_W, CF_NONE);
            } else {
                /* a malformed record stands in its block's last iteration between the repeats of
                   T's second and third records: the two before it are held back as T's when it
                   latches the frame, the one after it comes to a latched frame, and no later
                   image covers either, so the encoder's world holds them only through the latch */
                for (k = 0; k < 3; k++) {
                    if (fault && it == 4 && k == 2) cells_rec(&y, grid, 1, 12, 1, fault);
                    cells_rec(&y, grid, run[k].x, run[k].z, run[k].n, CF_NONE);
                }
                if (it == 3) cells_rec(&y, grid, 7, 7, 2, CF_NONE);
                if (it == 4 && !fault) cells_rec(&y, grid, 1, 12, 1, CF_NONE);
            }
            cells_bracket(&y, 0);
            for (k = 0; k < 3; k++) {
                for (c = 0; c < run[k].n; c++) {
                    unsigned char* cell = grid + ((size_t)run[k].z * CF_W + run[k].x + c) * REC_CELL_BYTES;
                    if (k == 2) {
                        cell[0] = (unsigned char)(it & 1 ? c + 1 : 0);
                        cell[1] = 0;
                    } else {
                        cell[(it + c) % REC_CELL_BYTES] ^= (unsigned char)(1u << (c & 7));
                    }
                }
                cells_rec(&y, grid, run[k].x, run[k].z, run[k].n, CF_NONE);
            }
            cells_bracket(&y, 1);
        }
        o = reccodec_stream_block(&st, y.p, y.n, &flags, &body, &bn);
        snprintf(line, sizeof line, "cells block %u: %s", block, what[block]);
        ok &= expect(line, o, fault || block == 13 ? RECC_DECLINED : block == 16 ? RECC_MISMATCH : RECC_SECTIONS);
        ok &= expect("  a reader decodes it back", reader_check(rd, &w, p, flags, body, bn,
                                                                  o == RECC_SECTIONS ? st.sec.n : y.n,
                                                                  y.p, y.n, st.dctx), 1);
        ok &= expect("  the encoder's grid is the reader's", world_same(&w, st.enc), 1);
        if (o == RECC_SECTIONS && block == 1) {
            static const unsigned char want[6] = { 1, 1, 1, 0, 0, 1 };
            int got = cells_flags(&st.sec, flag, &nflag, &emptied);
            ok &= expect("  the tick input's repeats flagged, the rest coded, slot A emptied",
                         got && nflag == 6 && !memcmp(flag, want, 6) && emptied, 1);
        }
        if (o == RECC_SECTIONS && block == 12)
            ok &= expect("  the images after it coded",
                         cells_flags(&st.sec, flag, &nflag, &emptied) && nflag == 1 && !flag[0], 1);
        if (o == RECC_SECTIONS && block == 15) {
            /* scheme 1 restarted G unknown: each of the run's cells would be sym 0x3F and its
               13 bytes in the field columns */
            int none = 1;
            sec_bounds(st.sec.p, RCS_CELLS, &a, &e);
            ok &= expect("  restarting flagged RBF_FRAME_RESET", !!(flags & RBF_FRAME_RESET), 1);
            if (frame_regions(st.sec.p, a, e, &tab, col) != CC_N) none = 0;
            else if (col[CC_SYM + 1] - col[CC_SYM] != run[0].n) none = 0;
            else
                for (k = 0; k < run[0].n; k++) none &= st.sec.p[col[CC_SYM] + k] == 0;
            if (none) none = col[CC_F0 + 6] == col[CC_F0];
            ok &= expect("  the run's cells are sym 0, and no field column holds a byte", none, 1);
            ok &= expect("  its first iteration's images are coded: T is empty after a restart",
                         cells_flags(&st.sec, flag, &nflag, &emptied) && nflag == 1 && !flag[0], 1);
        }
        if (o == RECC_SECTIONS && block == 17) {
            keep.n = 0;
            rb_put(&keep, st.sec.p, st.sec.n);
        }
    }
    /* block 17 again, from a restart whose world is not the codec's grid, or has none */
    fw = world_view(&w);
    fw.gw = p->gw + 1;
    reccodec_restart(rd, p, w.unit, w.live, w.pool, w.pcount, w.phw, &fw);
    why[0] = 0;
    ok &= expect("cells: a reader refuses a restart from a world of another grid",
                 !reccodec_valid(rd) && !reccodec_decode(rd, keep.p, keep.n, &out, why, sizeof why) &&
                 strstr(why, "no frame to continue") != NULL, 1);
    fw = world_view(&w);
    fw.grid = NULL;
    reccodec_restart(rd, p, w.unit, w.live, w.pool, w.pcount, w.phw, &fw);
    why[0] = 0;
    ok &= expect("cells: and from a world with no grid",
                 !reccodec_valid(rd) && !reccodec_decode(rd, keep.p, keep.n, &out, why, sizeof why) &&
                 strstr(why, "no frame to continue") != NULL, 1);
    reccodec_restart(rd, p, w.unit, w.live, w.pool, w.pcount, w.phw, NULL);
    ok &= expect("cells: and from no world", reccodec_valid(rd), 0);
    reccodec_stream_end(&st);
    reccodec_free(rd);
    rb_free(&y);
    rb_free(&keep);
    rb_free(&out);
    world_free(&w);
    return ok;
}

/* An RK_ORDERS of nodes [first, first + k) of list `list` of `slot`, `total` nodes long: node j's
   dwords a function of (id, j) alone, its timer +0x0A `tick` on the head node, `pad` its byte 3 */
static void ord_rec(RecBytes* b, unsigned slot, unsigned list, unsigned total, unsigned first,
                    unsigned k, uint32_t id, uint32_t tick, unsigned pad)
{
    static unsigned char body[8 + REC_ORDERS_CHUNK * REC_ORDER_IMAGE];
    unsigned j, d;
    body[0] = (unsigned char)slot;
    body[1] = (unsigned char)(slot >> 8);
    body[2] = (unsigned char)list;
    body[3] = (unsigned char)pad;
    body[4] = (unsigned char)total;
    body[5] = (unsigned char)(total >> 8);
    body[6] = (unsigned char)first;
    body[7] = (unsigned char)(first >> 8);
    for (j = 0; j < k; j++) {
        unsigned char* n = body + 8 + (size_t)j * REC_ORDER_IMAGE;
        for (d = 0; d < REC_ORDER_IMAGE / 4; d++)
            put32(n + 4 * d, (id * 7919u + first + j) * 2654435761u ^ d * 40503u);
        n[REC_ORDER_BYTES] = n[REC_ORDER_BYTES + 1] = 0;
        if (!(first + j)) memcpy(n + 0x0A, &tick, 4);
    }
    rec_put(b, RK_ORDERS, body, 8 + k * REC_ORDER_IMAGE);
}

/* a list of `total` nodes in as many records as the capture makes (tagpu_recwriter.c list_put) */
static void ord_list(RecBytes* b, unsigned slot, unsigned list, unsigned total, uint32_t id,
                     uint32_t tick)
{
    unsigned first = 0;
    do {
        unsigned k = total - first < REC_ORDERS_CHUNK ? total - first : REC_ORDERS_CHUNK;
        ord_rec(b, slot, list, total, first, k, id, tick, 0);
        first += k;
    } while (first < total);
}

/* Block `n` of outcomes_orders: what each block's records are is in its `what` below. */
static void ord_block(RecBytes* b, unsigned n, uint32_t* gt)
{
    unsigned it, t;
    b->n = 0;
    for (it = 0; it < (n >= 7 ? 1u : 4u); it++, (*gt)++) {
        rec_put(b, RK_TICK, gt, 4);
        switch (n) {
        case 0: case 1: case 3:
            ord_list(b, 1, 0, 2, 1, *gt);
            ord_list(b, 1, 1, 1, 2, *gt);
            ord_list(b, 5, 0, n == 0 && it == 2 ? 2 : 3, 3, *gt);
            ord_list(b, 3, 0, n == 0 && it == 0 ? 1 : 0, 4, *gt);
            if (n == 0 && it == 3) ord_list(b, 2, 0, 1, 5, *gt);
            if (n == 0 && it % 2 == 0) ord_list(b, 9, 0, 600, 6, *gt);
            break;
        case 2:
            ord_list(b, 1, 0, 2, 1, *gt);
            if (it == 2) ord_rec(b, 4, 0, 1, 0, 1, 7, *gt, 1);
            break;
        case 4:
            ord_list(b, 1, 0, 2, 1, *gt);
            if (it == 1) ord_rec(b, 4, 0, 2, 1, 2, 7, *gt, 0);
            break;
        case 5:
            ord_list(b, 1, 0, 2, 1, *gt);
            if (it == 3) ord_rec(b, 64, 0, 1, 0, 1, 7, *gt, 0);
            break;
        case 6:
            /* no list: the arena starts empty at block 7 */
            break;
        case 7:
            /* places of 1..361 nodes: 65,341 of the arena's 65,536; then 362 does not fit, and
               361 fits its place again */
            for (t = 1; t <= 362; t++) ord_list(b, 1, 0, t, 1, 0);
            ord_list(b, 1, 0, 362, 1, 0);
            ord_list(b, 1, 0, 361, 1, 0);
            ord_list(b, 1, 0, 361, 1, 0);
            break;
        default:
            /* 195 nodes fill the arena to exactly 65,536; 200 more do not fit */
            ord_list(b, 1, 0, 361, 1, 0);
            ord_list(b, 3, 0, 195, 8, 0);
            ord_list(b, 2, 0, 200, 9, 0);
            break;
        }
    }
}

/* The orders frame: its codings carry kept lists across blocks, lists that change length, an
   emptied list and a list of more than REC_ORDERS_CHUNK nodes; a record it cannot carry declines
   the block after its iterations; masks against a list the decoder does not hold are refused; and
   the arena keeps a list exactly while it holds at most 65,536 nodes, alike on both sides. */
static int outcomes_orders(const RecCodecParams* p)
{
    static const int want[10] = { RECC_SECTIONS, RECC_SECTIONS, RECC_DECLINED, RECC_SECTIONS,
                                  RECC_DECLINED, RECC_DECLINED, RECC_SECTIONS, RECC_SECTIONS,
                                  RECC_SECTIONS, RECC_SECTIONS };
    static const char* what[10] = {
        "orders: SECTIONS, kept, resized, emptied and 600-node lists",
        "orders: SECTIONS, the lists re-sent against the kept ones",
        "orders: DECLINED, a list whose pad byte is not zero",
        "orders: SECTIONS, restarting after it",
        "orders: DECLINED, a record past its list's nodes",
        "orders: DECLINED, a list of a slot past RT_SLOTS",
        "orders: SECTIONS, restarting after it with no list",
        "orders: SECTIONS, the arena filled to 65,341 nodes",
        "orders: SECTIONS, the arena filled to exactly 65,536",
        "orders: SECTIONS, a list past the arena carried whole" };
    RecStream st;
    RecCodec *rd, *fresh;
    RecBytes y, out;
    World w;
    uint32_t gt = 1;
    unsigned n, flags, ok = 1;
    const unsigned char* body;
    size_t bn;
    char why[256];
    memset(&w, 0, sizeof w);
    memset(&y, 0, sizeof y);
    memset(&out, 0, sizeof out);
    if (!reccodec_stream_begin(&st, p) || !(rd = reccodec_new(p)) || !(fresh = reccodec_new(p)))
        return 0;
    world_reset(&w, p->nslots, p->nproj, p->gw, p->gh);
    for (n = 0; n < 10; n++) {
        RecSecHead h;
        int o;
        ord_block(&y, n, &gt);
        o = reccodec_stream_block(&st, y.p, y.n, &flags, &body, &bn);
        ok &= expect(what[n], o, want[n]);
        memset(&h, 0, sizeof h);
        if (o == RECC_SECTIONS) memcpy(&h, st.sec.p, sizeof h);
        if (n == 1) {
            ok &= expect("  the re-sent lists take masks: under 256 bytes", h.len[RCS_ORDERS] < 256, 1);
            RecFrameWorld v = world_view(&w);
            reccodec_restart(fresh, p, w.unit, w.live, w.pool, w.pcount, w.phw, &v);
            ok &= expect("  a decoder that holds no kept lists refuses it",
                         reccodec_decode(fresh, st.sec.p, st.sec.n, &out, why, sizeof why) == 0 &&
                         strstr(why, "kept list") != NULL, 1);
        }
        if (n == 9)
            ok &= expect("  the 200-node list whole, the 361 and 195 kept",
                         h.len[RCS_ORDERS] >= 200 * REC_ORDER_IMAGE &&
                         h.len[RCS_ORDERS] < 200 * REC_ORDER_IMAGE + 1024, 1);
        ok &= expect("  a reader decodes it back",
                     reader_check(rd, &w, p, flags, body, bn, o == RECC_SECTIONS ? st.sec.n : y.n,
                                  y.p, y.n, st.dctx), 1);
    }
    reccodec_stream_end(&st);
    reccodec_free(rd);
    reccodec_free(fresh);
    rb_free(&y);
    rb_free(&out);
    world_free(&w);
    return ok;
}

/* the events frame's outcomes: a payload of every event kind, two iterations of it, in blocks */

enum { EV_PLAIN, EV_PAD, EV_NO_DEATH, EV_NESTED, EV_ROOT };

/* One iteration's events, every kind, the same each time it runs: the second run of an
   iteration repeats the first's names, texts, sprays, script arguments and sites. */
static void ev_events(RecBytes* b, const uint32_t units[][NDW], int variant)
{
    static const char longname[REC_NAME_MAX + 1] = "a_script_name_of_thirty_two_byte";
    unsigned char e[12 + IMG];
    unsigned i;
    memset(e, 0, sizeof e);
    put32(e, 4);                                         /* a create */
    e[4] = 12; e[6] = 1; e[7] = 1; put32(e + 8, 0x10000); put32(e + 12, 0x500000); e[24] = 3;
    rec_put(b, RK_EV_CREATE, e, 28);
    memset(e, 0, 32);
    put32(e, 5);                                         /* a network create */
    e[4] = 13; put32(e + 8, 77); put32(e + 12, 0x600000);
    rec_put(b, RK_EV_NET_CREATE, e, 32);
    for (i = 0; i < 3; i++) {                            /* shots: a target, none, no shooter */
        uint32_t sh = i == 2 ? 0xFFFF : 2, tg = i == 1 ? 0xFFFF : i == 2 ? 2 : 3, k;
        const unsigned char* su = (const unsigned char*)units[1];
        memset(e, 0, 36);
        e[0] = 0x0D;
        for (k = 0; k < 3; k++) put32(e + 1 + 4 * k, get32(su + 0x6A + 4 * k) + 9 * k + 5);
        for (k = 0; k < 3; k++) put32(e + 0xD + 4 * k, 0x00300000u + k);
        e[0x19] = 7; e[0x1A] = 1; e[0x1C] = 0x40;
        e[0x1F] = (unsigned char)tg; e[0x20] = (unsigned char)(tg >> 8);
        e[0x21] = (unsigned char)sh; e[0x22] = (unsigned char)(sh >> 8);
        e[0x23] = 1;
        rec_put(b, RK_EV_SHOT, e, 36);
    }
    for (i = 0; i < 4; i++) {                            /* starts: a script again, another, a
                                                            nonzero tail, a unit past the slots */
        memset(e, 0, 28);
        put32(e, i == 3 ? 9999 : 2);
        put32(e + 4, i == 2 ? 9 : 7);
        put32(e + 8, i == 2 ? 6 : 2);
        put32(e + 12, 5); put32(e + 16, 6 + i); put32(e + 20, i == 2 ? 3 : 0); put32(e + 24, i == 2 ? 0xFFFFFFFFu : 0);
        rec_put(b, RK_EV_START, e, 28);
    }
    for (i = 0; i < 3; i++) {                            /* named starts: a name, it again, the
                                                            longest name */
        unsigned n = i == 2 ? REC_NAME_MAX : 8;
        memset(e, 0, 24 + REC_NAME_MAX);
        put32(e, 3);
        e[4] = 1; e[5] = 1; put32(e + 8, 0xFFFFFFF0u);
        memcpy(e + 24, i == 2 ? longname : "Activate", n);
        rec_put(b, RK_EV_START_NAMED, e, 24 + n);
    }
    for (i = 0; i < 2; i++) {                            /* replies: a text, it again */
        memset(e, 0, 17);
        put32(e, 3); put32(e + 4, 2); e[8] = 1; e[9] = 1; memcpy(e + 12, "hello", 5);
        rec_put(b, RK_EV_REPLY, e, 17);
    }
    for (i = 0; i < 4; i++) {                            /* sprays: raw, the same, a new point, a
                                                            unit past the slots */
        memset(e, 0, 48);
        put32(e, i == 3 ? 9999 : 2);
        put32(e + 4, 1); put32(e + 8, 0x100 + (i == 2) * 3); put32(e + 12, 0x200); put32(e + 16, 0x300);
        put32(e + 20, 11); put32(e + 44, 2);
        rec_put(b, RK_EV_SPRAY, e, 48);
    }
    /* a death whose explosion calls a feature function and draws, with a death nested in it */
    memset(e, 0, sizeof e);
    put32(e, 3); put32(e + 4, 2); e[8] = 1; e[9] = 0xFE; e[10] = 4;
    memcpy(e + 12, units[2], IMG);
    put32(e + 12 + 0x10, 0x1234); put32(e + 12 + 0x140, 9);
    rec_put(b, RK_EV_DEATH, e, 12 + IMG);
    memset(e, 0, 20);
    e[0] = FR_DIE; e[1] = REC_PHASE_LATE; put32(e + 4, 0x401000); put32(e + 8, 0x300000); put32(e + 16, 1);
    rec_put(b, RK_EV_FEATURE, e, 20);
    memset(e, 0, 12);
    e[0] = FS_BURN; put32(e + 4, 30); put32(e + 8, 17);
    rec_put(b, RK_EV_DRAW, e, 12);
    memset(e, 0, 8);
    e[0] = FR_DIE; put32(e + 4, 1);
    rec_put(b, RK_EV_FEATURE_END, e, 8);
    for (i = 0; i < (variant == EV_NESTED ? REC_DEATHS_MAX : 1u); i++) {
        memset(e, 0, sizeof e);
        put32(e, 2);
        e[8] = 10;
        memcpy(e + 12, units[1], IMG);
        rec_put(b, RK_EV_DEATH, e, 12 + IMG);
    }
    for (i = 0; i < (variant == EV_NESTED ? REC_DEATHS_MAX : 1u); i++) {
        memset(e, 0, 4);
        put32(e, 2);
        rec_put(b, RK_EV_DEATH_END, e, 4);
    }
    put32(e, 3);
    rec_put(b, RK_EV_DEATH_END, e, 4);
    if (variant == EV_NO_DEATH) rec_put(b, RK_EV_DEATH_END, e, 4);
    for (i = 0; i < 2; i++) {                            /* the feature tick, at its one site */
        memset(e, 0, 8);
        e[0] = variant == EV_ROOT ? FR_ROOTS : FR_TICK; put32(e + 4, 0x495580);
        rec_put(b, RK_EV_FEATURE, e, 8);
        memset(e, 0, 8);
        rec_put(b, RK_EV_FEATURE_END, e, 8);
    }
    memset(e, 0, 16);
    put32(e, 2); put32(e + 4, 4);
    rec_put(b, RK_EV_BUILT, e, 8);
    memset(e, 0, 8);
    put32(e, 4); e[4] = 5; e[5] = variant == EV_PAD;
    rec_put(b, RK_EV_STATE, e, 8);
    memset(e, 0, 16);
    put32(e, 0x100); put32(e + 4, 0x200); put32(e + 8, 0x300); e[12] = 44;
    rec_put(b, RK_EV_INTERCEPT, e, 16);
}

/* two iterations: three units made, the events; the events again */
static void ev_block(RecBytes* b, uint32_t units[][NDW], uint32_t* gt, int variant)
{
    unsigned s, it;
    b->n = 0;
    for (it = 0; it < 2; it++, (*gt)++) {
        rec_put(b, RK_TICK, gt, 4);
        if (!it)
            for (s = 1; s <= 3; s++) {
                memset(units[s], 0, IMG);
                units[s][0] = 0x2000 + s;
                lane_add(units[s], REC_LR_TYPE, 20 + s, 1);
                lane_add(units[s], REC_LR_X, 0x80000u * s, 1);
                lane_add(units[s], REC_LR_Z, 0x40000u * s, 1);
                lane_add(units[s], REC_LR_LAYER, 1, 1);
                rec_unit(b, s, units[s]);
            }
        ev_events(b, (const uint32_t(*)[NDW])units, variant);
    }
}

static int outcomes_events(const RecCodecParams* p)
{
    /* the events frame's columns that the refusals below mutate (tagpu_recframe_events.c, C_*) */
    enum { COL_ST_RZ = 7, COL_NM_NAME = 10, COL_SP_MODE = 15, COL_DT_MASK = 20, COL_FT_HEAD = 22,
           COL_FT_SAME = 23 };
    static const int variant[10] = { EV_PLAIN, EV_PLAIN, EV_PAD, EV_PLAIN, EV_NO_DEATH, EV_PLAIN,
                                     EV_NESTED, EV_PLAIN, EV_ROOT, EV_PLAIN };
    static const int want[10] = { RECC_SECTIONS, RECC_SECTIONS, RECC_DECLINED, RECC_SECTIONS,
                                  RECC_DECLINED, RECC_SECTIONS, RECC_DECLINED, RECC_SECTIONS,
                                  RECC_DECLINED, RECC_SECTIONS };
    static const char* what[10] = {
        "events: SECTIONS, every kind", "events: SECTIONS, continuing on the frame's state",
        "events: DECLINED, a pad that is not zero", "events: SECTIONS, restarting after it",
        "events: DECLINED, a death's end with no death open", "events: SECTIONS, restarting after it",
        "events: DECLINED, deaths nested past REC_DEATHS_MAX", "events: SECTIONS, restarting after it",
        "events: DECLINED, a feature root past FR_ROOTS", "events: SECTIONS, restarting after it" };
    /* each sets (or ORs in) one byte of a column, at the column's first event of its kind */
    static const struct { unsigned col, at, val, or_in; const char* refusal; } mut[] = {
        { COL_ST_RZ, 0, 2, 0, "argument flag" },
        { COL_NM_NAME, 0, 5, 0, "past the names" },
        { COL_SP_MODE, 0, 0, 0, "spray mode" },
        { COL_DT_MASK, REC_UNIT_BITS - 1, 0x80, 1, "mask names dword" },
        { COL_FT_HEAD, 0, FR_ROOTS, 0, "feature root" },
        { COL_FT_SAME, 0, 1, 0, "same-site" } };
    static uint32_t units[4][NDW];
    RecStream st;
    RecCodec* rd;
    World w, pre;
    RecFrameWorld pv;
    RecBytes y, first, keep, out;
    uint32_t gt = 100;
    unsigned block, flags, i, ok = 1;
    const unsigned char* body;
    size_t bn, a, e, tab, col[RECF_COLS_MAX + 1];
    char why[256], line[128];
    memset(&w, 0, sizeof w);
    memset(&pre, 0, sizeof pre);
    memset(&y, 0, sizeof y);
    memset(&first, 0, sizeof first);
    memset(&keep, 0, sizeof keep);
    memset(&out, 0, sizeof out);
    if (!reccodec_stream_begin(&st, p) || !(rd = reccodec_new(p))) return 0;
    world_reset(&w, p->nslots, p->nproj, p->gw, p->gh);
    world_reset(&pre, p->nslots, p->nproj, p->gw, p->gh);
    for (block = 0; block < 10; block++) {
        int o;
        ev_block(&y, units, &gt, variant[block]);
        memcpy(pre.unit, w.unit, (size_t)w.nslots * IMG);
        memcpy(pre.live, w.live, w.nslots);
        memcpy(pre.pool, w.pool, (size_t)(w.nproj ? w.nproj : 1) * PIMG);
        pre.pcount = w.pcount;
        pre.phw = w.phw;
        memcpy(pre.grid, w.grid, (size_t)w.gw * w.gh * REC_CELL_BYTES);
        o = reccodec_stream_block(&st, y.p, y.n, &flags, &body, &bn);
        snprintf(line, sizeof line, "block %u: %s", block, what[block]);
        ok &= expect(line, o, want[block]);
        if (o == RECC_SECTIONS) {
            ok &= expect("  flagged RBF_FRAME_RESET exactly when it restarts", !!(flags & RBF_FRAME_RESET),
                         block != 1);
            sec_bounds(st.sec.p, RCS_EVENTS, &a, &e);
            if (block == 0) {
                rb_put(&first, st.sec.p + a, e - a);
                ok &= expect("  the sprays code raw, the same, a new point, raw; then 1, 0, 1, 2",
                             frame_regions(st.sec.p, a, e, &tab, col) > COL_FT_SAME &&
                             col[COL_SP_MODE + 1] - col[COL_SP_MODE] == 8 &&
                             !memcmp(st.sec.p + col[COL_SP_MODE], "\2\0\1\2\1\0\1\2", 8), 1);
                ok &= expect("  the names code a literal, a hit, a literal; then three hits",
                             col[COL_NM_NAME + 1] - col[COL_NM_NAME] == 6 &&
                             !memcmp(st.sec.p + col[COL_NM_NAME], "\0\1\0\1\1\2", 6), 1);
            } else if (block != 1) {
                ok &= expect("  the frame starts empty: the events section is block 0's",
                             e - a == first.n && !memcmp(st.sec.p + a, first.p, first.n), 1);
            }
            if (block == 9) {
                keep.n = 0;
                rb_put(&keep, st.sec.p, st.sec.n);
            }
        }
        ok &= expect("  a reader decodes it back", reader_check(rd, &w, p, flags, body, bn,
                                                                  o == RECC_SECTIONS ? st.sec.n : y.n,
                                                                  y.p, y.n, st.dctx), 1);
    }
    /* the last block from its reset, each mutation refused by name */
    pv = world_view(&pre);
    sec_bounds(keep.p, RCS_EVENTS, &a, &e);
    ok &= expect("events: the last block's columns read", frame_regions(keep.p, a, e, &tab, col) > COL_FT_SAME, 1);
    for (i = 0; i < sizeof mut / sizeof mut[0]; i++) {
        unsigned char* c;
        int got;
        y.n = 0;
        rb_put(&y, keep.p, keep.n);
        c = y.p + col[mut[i].col] + mut[i].at;
        *c = (unsigned char)(mut[i].or_in ? *c | mut[i].val : mut[i].val);
        reccodec_restart(rd, p, pre.unit, pre.live, pre.pool, pre.pcount, pre.phw, &pv);
        why[0] = 0;
        got = reccodec_decode(rd, y.p, y.n, &out, why, sizeof why);
        snprintf(line, sizeof line, "events: refused, %s", mut[i].refusal);
        ok &= expect(line, !got && strstr(why, mut[i].refusal) != NULL, 1);
        if (got || !strstr(why, mut[i].refusal)) printf("  decoder: %s\n", why);
    }
    reccodec_restart(rd, p, pre.unit, pre.live, pre.pool, pre.pcount, pre.phw, &pv);
    ok &= expect("events: the unmutated block from its reset decodes",
                 reccodec_decode(rd, keep.p, keep.n, &out, why, sizeof why), 1);
    reccodec_stream_end(&st);
    reccodec_free(rd);
    rb_free(&y);
    rb_free(&first);
    rb_free(&keep);
    rb_free(&out);
    world_free(&w);
    world_free(&pre);
    return ok;
}

/* The state frame's columns in tagpu_recframe_state.c's order, and its scheme. Every crafted
   section below that must be refused has a twin that must be accepted, which differs from it in
   the one value under test: that is what shows these indexes name the columns they mean. */
enum { SC_GFULL, SC_GM, SC_GMASK, SC_GMETH, SC_GVAL, SC_GSTEPS, SC_GRAW, SC_PL, SC_PM, SC_PMASK,
       SC_PIMG, SC_PVAL, SC_PRAW, SC_WI, SC_WM, SC_WV, SC_WRAW, SC_FM, SC_FV, SC_N };
#define SC_SCHEME 1
static const char* const SC_NAME[SC_N] = { "gfull", "gm", "gmask", "gmeth", "gval", "gsteps",
                                           "graw", "pl", "pm", "pmask", "pimg", "pval", "praw",
                                           "wi", "wm", "wv", "wraw", "fm", "fv" };

enum { SF_NONE, SF_PPAD, SF_PLAYER, SF_WRECK, SF_FLEN, SF_GPAD, SF_PBIT };

/* The state a capture would record: the globals, players 0 and 3, three wreck records and the
   feature heads, changed each iteration so that every column is used -- a seed stepped by the
   engine's RNG (once past RECC_PM_NEAR steps after a win, once past the cap), a second and a
   third seed in the same iteration (the third past the method-1 bound), a counter, a dword that
   jumps, a player the block never images whole, a wreck record met first by a delta, and the
   cursor wrapping at the grid's last cell. */
typedef struct SSim {
    uint32_t g[REC_GLOBALS_DWORDS];
    uint32_t pv[2][REC_PLAYER_DWORDS];
    unsigned char wr[3][REC_WRECK_BYTES];
    uint32_t feat[4];
    unsigned n;
} SSim;

static uint32_t ss_pm(uint32_t s, unsigned k)
{
    while (k--) {
        uint32_t t = (uint32_t)((uint64_t)(s % 0x7FFFFFFFu) * 16807u % 0x7FFFFFFFu);
        s = t ? t : 0x7FFFFFFFu;
    }
    return s;
}

static void ss_globals_delta(RecBytes* b, const uint32_t* old, const uint32_t* now, int fault)
{
    unsigned char body[REC_GLOBALS_DELTA_HEAD + 4 * REC_GLOBALS_DWORDS];
    unsigned d, nb = 0;
    memset(body, 0, sizeof body);
    for (d = 0; d < REC_GLOBALS_DWORDS; d++)
        if (old[d] != now[d]) {
            body[d >> 3] |= (unsigned char)(1u << (d & 7));
            memcpy(body + REC_GLOBALS_DELTA_HEAD + 4 * nb++, &now[d], 4);
        }
    if (fault) body[REC_GLOBALS_DELTA_HEAD - 1] = 1;
    rec_put(b, RK_GLOBALS_DELTA, body, REC_GLOBALS_DELTA_HEAD + 4 * nb);
}

static void ss_player_delta(RecBytes* b, unsigned p, const uint32_t* old, const uint32_t* now,
                            int fault)
{
    unsigned char body[REC_PLAYER_DELTA_HEAD + 4 * (REC_PLAYER_DWORDS + 1)];
    unsigned i, nb = 0;
    memset(body, 0, sizeof body);
    body[0] = (unsigned char)p;
    for (i = 0; i < REC_PLAYER_DWORDS; i++)
        if (old[i] != now[i]) {
            body[2 + (i >> 3)] |= (unsigned char)(1u << (i & 7));
            memcpy(body + REC_PLAYER_DELTA_HEAD + 4 * nb++, &now[i], 4);
        }
    if (fault == SF_PPAD) body[REC_PLAYER_DELTA_HEAD - 1] = 1;
    if (fault == SF_PBIT) {
        body[2 + (REC_PLAYER_DWORDS >> 3)] |= (unsigned char)(1u << (REC_PLAYER_DWORDS & 7));
        nb++;
    }
    rec_put(b, RK_PLAYER_DELTA, body, REC_PLAYER_DELTA_HEAD + 4 * nb);
}

/* six iterations of the state, `fault` in the fourth; the first block's first iteration images
   the globals and player 0 whole */
static void ss_block(RecBytes* b, SSim* s, uint32_t* gt, int first, int fault)
{
    static const unsigned steps[6] = { 3, 40, 100, 9, 20000, 16 };
    static const unsigned wrecks[3] = { 5, 6, 63 };
    unsigned it, j;
    b->n = 0;
    for (it = 0; it < 6; it++, (*gt)++, s->n++) {
        uint32_t og[REC_GLOBALS_DWORDS], op[2][REC_PLAYER_DWORDS];
        unsigned char body[4 + REC_PLAYER_IMAGE];
        rec_put(b, RK_TICK, gt, 4);
        memcpy(og, s->g, sizeof og);
        memcpy(op, s->pv, sizeof op);
        s->g[0] = ss_pm(s->g[0], steps[s->n % 6]);
        s->g[1]++;
        s->g[2] = ss_pm(s->g[2], 5);
        s->g[3] = ss_pm(s->g[3], 7);
        s->g[4] += 0x9E3779B9u * (s->n + 1);
        if (s->n & 1) s->g[REC_GLOBALS_DWORDS - 1] += 3;
        s->pv[0][5] += 2;
        s->pv[0][6] = *gt;
        if (s->n % 3 == 0) s->pv[0][100] ^= 0x10001u;
        s->pv[1][7] += 1000u * s->n + 1;
        if (first && !it) {
            rec_put(b, RK_GLOBALS, s->g, sizeof s->g);
            memset(body, 0, 4);
            memcpy(body + 4, s->pv[0], REC_PLAYER_IMAGE);
            rec_put(b, RK_PLAYER, body, sizeof body);
        } else {
            ss_globals_delta(b, og, s->g, fault == SF_GPAD && it == 3);
            ss_player_delta(b, 0, op[0], s->pv[0], it == 3 ? fault : SF_NONE);
        }
        ss_player_delta(b, 3, op[1], s->pv[1], SF_NONE);
        for (j = 0; j < 3; j++)
            if ((s->n + j) % 2 == 0) {
                unsigned char* w = s->wr[j];
                w[2 * (s->n % 24)] ^= (unsigned char)(s->n + 1);
                w[47] = (unsigned char)j;
                body[0] = (unsigned char)wrecks[j];
                body[1] = body[2] = body[3] = 0;
                memcpy(body + 4, w, REC_WRECK_BYTES);
                rec_put(b, RK_WRECK, body, 4 + REC_WRECK_BYTES);
            }
        s->feat[3] = s->feat[3] ? s->feat[3] - 1 : 16 * 16 - 1;
        if (s->n % 4 == 0) s->feat[0] = s->feat[0] * 7 + 1;
        if (s->n % 5 == 0) s->feat[3] = 2;
        rec_put(b, RK_FEATURES, s->feat, fault == SF_FLEN && it == 3 ? 12 : 16);
        if (it == 3 && fault == SF_PLAYER) {
            memset(body, 0, sizeof body);
            body[0] = REC_PLAYERS;
            rec_put(b, RK_PLAYER, body, sizeof body);
        }
        if (it == 3 && fault == SF_WRECK) {
            memset(body, 0, 4 + REC_WRECK_BYTES);
            body[0] = 64;
            rec_put(b, RK_WRECK, body, 4 + REC_WRECK_BYTES);
        }
    }
}

/* A crafted state section, its columns and the kinds its records are read as (0: the next
   iteration), decoded by the frame from a restart: 1 accepted, 0 refused. */
typedef struct SCase {
    RecBytes c[SC_N];
    unsigned kind[16], nk;
} SCase;

static void sc_reset(SCase* s)
{
    unsigned i;
    for (i = 0; i < SC_N; i++) rb_clear(&s->c[i]);
    s->nk = 0;
}

static void sc_kind(SCase* s, unsigned kind) { s->kind[s->nk++] = kind; }

static int sc_run(const RecCodecParams* p, SCase* s)
{
    const RecFrameOps* f = &recframe_state;
    void* x = f->make(p);
    unsigned char* body = (unsigned char*)malloc(RECF_BODY_MAX);
    RecBytes sec;
    RecFrameAt a;
    char why[256];
    unsigned i, len;
    int ok;
    if (!x || !body) {
        fprintf(stderr, "codectest: no memory for the state frame\n");
        exit(2);
    }
    memset(&sec, 0, sizeof sec);
    memset(&a, 0, sizeof a);
    a.nslots = p->nslots;
    f->restart(x, NULL);
    ok = recf_write(&sec, SC_SCHEME, s->c, SC_N) && f->dec_open(x, sec.p, sec.n, 1, why, sizeof why) &&
         f->dec_iter(x, &a, why, sizeof why);
    for (i = 0; ok && i < s->nk; i++)
        if (!s->kind[i]) {
            ok = f->dec_iter_end(x, &a, why, sizeof why);
            a.it++;
            ok = ok && f->dec_iter(x, &a, why, sizeof why);
        } else {
            ok = f->dec_rec(x, &a, s->kind[i], body, &len, why, sizeof why);
        }
    ok = ok && f->dec_iter_end(x, &a, why, sizeof why) && f->dec_close(x, why, sizeof why);
    f->drop(x);
    free(body);
    rb_free(&sec);
    return ok;
}

/* an RK_GLOBALS of n dwords, then a delta whose mask is bit d alone, by method `meth` with value
   v (the steps, the residual or the raw dword) */
static void sc_global(SCase* s, unsigned n, unsigned d, unsigned meth, uint32_t v)
{
    unsigned i;
    rb_varint(&s->c[SC_GFULL], n);
    for (i = 0; i < n; i++) rb_u32(&s->c[SC_GFULL], 1000 + i);
    sc_kind(s, RK_GLOBALS);
    rb_u8(&s->c[SC_GM], 1);
    rb_u8(&s->c[SC_GMASK], 1u << d);
    rb_u8(&s->c[SC_GMETH], meth);
    if (meth == 0) rb_varint(&s->c[SC_GVAL], v);
    else if (meth == 1) rb_varint(&s->c[SC_GSTEPS], v);
    else rb_u32(&s->c[SC_GRAW], v);
    sc_kind(s, RK_GLOBALS_DELTA);
}

/* `deltas` globals deltas of one changed dword by 1 step each, `per` to an iteration */
static void sc_steps(SCase* s, unsigned deltas, unsigned per)
{
    unsigned i;
    rb_varint(&s->c[SC_GFULL], 1);
    rb_u32(&s->c[SC_GFULL], 12345);
    sc_kind(s, RK_GLOBALS);
    for (i = 0; i < deltas; i++) {
        if (i && i % per == 0) sc_kind(s, 0);
        rb_u8(&s->c[SC_GM], i ? 0 : 1);
        if (!i) rb_u8(&s->c[SC_GMASK], 1);
        rb_u8(&s->c[SC_GMETH], 1);
        rb_varint(&s->c[SC_GSTEPS], 1);
        sc_kind(s, RK_GLOBALS_DELTA);
    }
}

/* a delta of player p, flag `flag`, whose mask is dword i alone, raw: the player is unknown */
static void sc_player(SCase* s, unsigned p, unsigned flag, unsigned i)
{
    unsigned char m[REC_PLAYER_BITS];
    memset(m, 0, sizeof m);
    m[i >> 3] = (unsigned char)(1u << (i & 7));
    rb_u8(&s->c[SC_PL], p);
    rb_u8(&s->c[SC_PM], flag);
    rb_put(&s->c[SC_PMASK], m, sizeof m);
    rb_u32(&s->c[SC_PRAW], 77);
    sc_kind(s, RK_PLAYER_DELTA);
}

static int outcomes_state(const RecCodecParams* p)
{
    static const char* const what[6] = {
        "DECLINED: a player delta whose pad is not zero",
        "DECLINED: an RK_PLAYER of player 10",
        "DECLINED: an RK_WRECK past the wreck pool",
        "DECLINED: an RK_FEATURES of 12 bytes",
        "DECLINED: a globals delta whose pad is not zero",
        "DECLINED: a player delta naming dword 130" };
    static const char* const cases[] = {
        "a globals image of 78 dwords", "of 0", "of 257",
        "a known global by method 1", "an unknown one", "method 3",
        "16,384 steps", "16,385", "0",
        "two globals an iteration by steps, then one", "three in one iteration",
        "a globals delta that repeats a mask before the first", "a mask of 33 bytes",
        "player 9", "player 10", "a player mask flag 2",
        "a player mask naming dword 129", "naming dword 130",
        "wreck record 63 of 64", "wreck record 64",
        "a feature mask 15", "a feature mask 16" };
    static const int want[] = { 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
    static SSim sim;
    size_t used[SC_N];
    RecStream st;
    RecCodec* rd;
    RecBytes y;
    World w;
    SCase s;
    uint32_t gt = 1;
    unsigned block, i, k, flags;
    const unsigned char* body;
    size_t bn;
    int ok = 1;
    memset(&sim, 0, sizeof sim);
    memset(&y, 0, sizeof y);
    memset(&w, 0, sizeof w);
    memset(&s, 0, sizeof s);
    memset(used, 0, sizeof used);
    sim.g[0] = sim.g[2] = sim.g[3] = 16807;
    if (!reccodec_stream_begin(&st, p) || !(rd = reccodec_new(p))) return 0;
    world_reset(&w, p->nslots, p->nproj, p->gw, p->gh);
    for (block = 0; block < 14; block++) {
        int fault = block >= 2 && block % 2 == 0 ? 1 + (int)(block - 2) / 2 : SF_NONE, o;
        char line[128];
        ss_block(&y, &sim, &gt, block == 0, fault);
        o = reccodec_stream_block(&st, y.p, y.n, &flags, &body, &bn);
        if (fault) snprintf(line, sizeof line, "state block %u: %s", block, what[fault - 1]);
        else snprintf(line, sizeof line, "state block %u: SECTIONS%s", block,
                      block == 0 ? ", the segment's first block" : block == 1 ? ", continuing" : ", restarting after it");
        ok &= expect(line, o, fault ? RECC_DECLINED : RECC_SECTIONS);
        if (o == RECC_SECTIONS) {
            RecCol col[SC_N];
            const RecBytes* sec = &st.enc->sec[RCS_STATE];
            if (recf_open(sec->p, sec->n, SC_SCHEME, SC_N, col, "state", NULL, 0))
                for (k = 0; k < SC_N; k++) used[k] += col[k].end - col[k].at;
        }
        ok &= expect("  a reader decodes it back", reader_check(rd, &w, p, flags, body, bn,
                                                                  o == RECC_SECTIONS ? st.sec.n : y.n,
                                                                  y.p, y.n, st.dctx), 1);
    }
    for (k = 0; k < SC_N; k++)
        if (!used[k]) printf("  the state frame's column %s was never used\n", SC_NAME[k]);
    for (k = 0; k < SC_N && used[k]; k++) {}
    ok &= expect("state: the sectioned blocks used every column", k == SC_N, 1);
    for (i = 0; i < sizeof want / sizeof want[0]; i++) {
        char line[128];
        sc_reset(&s);
        switch (i) {
        case 0: case 1: case 2:
            rb_varint(&s.c[SC_GFULL], i == 0 ? 78 : i == 1 ? 0 : 257);
            for (k = 0; k < (i == 0 ? 78u : i == 1 ? 0u : 257u); k++) rb_u32(&s.c[SC_GFULL], k);
            sc_kind(&s, RK_GLOBALS);
            break;
        case 3: sc_global(&s, 2, 1, 1, 5); break;
        case 4: sc_global(&s, 2, 5, 1, 5); break;
        case 5: sc_global(&s, 2, 1, 3, 5); break;
        case 6: sc_global(&s, 2, 1, 1, 16384); break;
        case 7: sc_global(&s, 2, 1, 1, 16385); break;
        case 8: sc_global(&s, 2, 1, 1, 0); break;
        case 9: sc_steps(&s, 3, 2); break;
        case 10: sc_steps(&s, 3, 3); break;
        case 11:
            rb_u8(&s.c[SC_GM], 0);
            sc_kind(&s, RK_GLOBALS_DELTA);
            break;
        case 12:
            rb_u8(&s.c[SC_GM], 33);
            for (k = 0; k < 33; k++) rb_u8(&s.c[SC_GMASK], 0);
            sc_kind(&s, RK_GLOBALS_DELTA);
            break;
        case 13: case 14:
            rb_u8(&s.c[SC_PL], i == 13 ? 9 : 10);
            for (k = 0; k < REC_PLAYER_IMAGE; k++) rb_u8(&s.c[SC_PIMG], k);
            sc_kind(&s, RK_PLAYER);
            break;
        case 15: sc_player(&s, 2, 2, 4); break;
        case 16: sc_player(&s, 2, 0, REC_PLAYER_DWORDS - 1); break;
        case 17: sc_player(&s, 2, 0, REC_PLAYER_DWORDS); break;
        case 18: case 19:
            rb_varint(&s.c[SC_WI], zz32(i == 18 ? 63 : 64));
            for (k = 0; k < REC_WRECK_BYTES; k++) rb_u8(&s.c[SC_WRAW], k);
            sc_kind(&s, RK_WRECK);
            break;
        case 20: case 21:
            rb_u8(&s.c[SC_FM], i == 20 ? 15 : 16);
            for (k = 0; k < 4; k++) rb_varint(&s.c[SC_FV], k);
            sc_kind(&s, RK_FEATURES);
            break;
        }
        snprintf(line, sizeof line, "state frame: %s %s", cases[i], want[i] ? "reads" : "is refused");
        ok &= expect(line, sc_run(p, &s), want[i]);
    }
    for (k = 0; k < SC_N; k++) rb_free(&s.c[k]);
    reccodec_stream_end(&st);
    reccodec_free(rd);
    rb_free(&y);
    world_free(&w);
    return ok;
}

/* the ledger rule's single-precision sum against known IEEE results: rounding to even, a carry
   into the exponent, cancellation to zero and to a denormal, denormals, the overflow */
static int f32_check(void)
{
    static const uint32_t t[][3] = {
        { 0x3FC00000u, 0x3FC00000u, 0x40400000u },   /* 1.5 + 1.5 = 3 */
        { 0x3F800000u, 0x33800000u, 0x3F800000u },   /* 1 + 2^-24: a tie, to even */
        { 0x3F800001u, 0x33800000u, 0x3F800002u },   /* 1+ulp + 2^-24: a tie, to even, up */
        { 0x3F7FFFFFu, 0x33800000u, 0x3F800000u },   /* a carry into the exponent */
        { 0x40490FDBu, 0xC0490FDBu, 0x00000000u },   /* x - x = +0 */
        { 0x00800000u, 0x80700000u, 0x00100000u },   /* a normal less a denormal: a denormal */
        { 0x00000001u, 0x00000001u, 0x00000002u },   /* two denormals */
        { 0x7F7FFFFFu, 0x7F7FFFFFu, 0x7F800000u },   /* the overflow: infinity */
        { 0x80000000u, 0x80000000u, 0x80000000u },   /* -0 + -0 = -0 */
        { 0x4B800000u, 0x3F800000u, 0x4B800000u },   /* 2^24 + 1: a tie, to even */
    };
    unsigned i, bad = 0;
    for (i = 0; i < sizeof t / sizeof t[0]; i++)
        if (reccodec_f32_add(t[i][0], t[i][1]) != t[i][2] || reccodec_f32_add(t[i][1], t[i][0]) != t[i][2]) {
            printf("  f32: %08x + %08x = %08x, not %08x\n", t[i][0], t[i][1], reccodec_f32_add(t[i][0], t[i][1]), t[i][2]);
            bad++;
        }
    return expect("ledger: single-precision sums round as IEEE does", bad, 0);
}

/* ---- outcomes: the lossy filter ---------------------------------------------------------------- */

#define LO_SLOTS 8
#define LO_HELD  50           /* a unit dword held whole                                         */
#define LO_PART  51           /* a unit dword held in its bits 0x0000FF00                        */
#define LO_NODE  3            /* a main list node's dword held whole                             */

static void lo_tick(RecBytes* b, unsigned t)
{
    unsigned char v[4];
    b->n = 0;
    put32(v, t);
    tlv_put(b, RK_TICK, v, 4);
}

static void lo_unit(RecBytes* b, unsigned s, const uint32_t* img)
{
    unsigned char r[4 + IMG];
    memset(r, 0, 4);
    r[0] = (unsigned char)s;
    memcpy(r + 4, img, IMG);
    tlv_put(b, RK_UNIT, r, sizeof r);
}

/* a delta of slot s naming the dwords d[0..n) with the values v[0..n) */
static void lo_delta(RecBytes* b, unsigned s, const unsigned* d, const uint32_t* v, unsigned n)
{
    unsigned char r[REC_UNIT_DELTA_HEAD + 4 * NDW];
    unsigned i;
    memset(r, 0, sizeof r);
    r[0] = (unsigned char)s;
    for (i = 0; i < n; i++) {
        r[2 + (d[i] >> 3)] |= (unsigned char)(1u << (d[i] & 7));
        put32(r + REC_UNIT_DELTA_HEAD + 4 * i, v[i]);
    }
    tlv_put(b, RK_UNIT_DELTA, r, REC_UNIT_DELTA_HEAD + 4 * n);
}

/* an order list of nodes images, one record */
static void lo_list(RecBytes* b, unsigned s, unsigned list, const unsigned char* img, unsigned n,
                    unsigned first, unsigned total)
{
    unsigned char r[8 + 4 * REC_ORDER_IMAGE];
    memset(r, 0, 8);
    r[0] = (unsigned char)s;
    r[2] = (unsigned char)list;
    r[4] = (unsigned char)total;
    r[6] = (unsigned char)first;
    memcpy(r + 8, img, (size_t)n * REC_ORDER_IMAGE);
    tlv_put(b, RK_ORDERS, r, 8 + n * REC_ORDER_IMAGE);
}

static void lo_event(RecBytes* b)
{
    unsigned char r[8] = { 1, 0, 0, 0, 7, 0, 0, 0 };
    tlv_put(b, RK_EV_STATE, r, 8);
}

/* the records of `out` from its byte `at`, as (kind, slot) pairs, into k[] and s[]: their count */
static unsigned lo_kinds(const RecBytes* out, unsigned* k, unsigned* s, unsigned cap)
{
    size_t at = 0;
    unsigned n = 0;
    while (at + 4 <= out->n && n < cap) {
        k[n] = get16(out->p + at);
        s[n] = get16(out->p + at + 4);
        at += (4 + (size_t)get16(out->p + at + 2) + 3) & ~(size_t)3;
        n++;
    }
    return n;
}

/* one iteration through the filter, the copy taking the exact records first, as the writer does */
static int lo_snap(RecLossy* L, Exact* e, const RecBytes* in, int refresh, int snapshot,
                   RecBytes* out)
{
    RecLossyExact x;
    out->n = 0;
    if (!exact_apply(e, in->p, in->n)) return -1;
    x.units = e->units;
    x.live = e->live;
    x.lists = e->lists;
    x.grid = e->grid;
    return reclossy_iteration(L, in->p, in->n, refresh, snapshot, &x, out);
}

static int lo_step(RecLossy* L, Exact* e, const RecBytes* in, int refresh, RecBytes* out)
{
    return lo_snap(L, e, in, refresh, 0, out);
}

/* ---- outcomes: the spray sessions ---------------------------------------------------------------- */

#define LO_SPRAYS 8           /* the sprays one test iteration holds at most                      */

/* an RK_EV_SPRAY of unit ID `id` at the point (x, y, z), its box `box` .. `box` + 5 */
static void lo_spray(RecBytes* b, unsigned id, int32_t x, int32_t y, int32_t z, unsigned box)
{
    unsigned char r[4 + REC_SPRAY_BODY];
    unsigned i;
    memset(r, 0, sizeof r);
    put32(r, id);
    put32(r + 8, (uint32_t)x);
    put32(r + 12, (uint32_t)y);
    put32(r + 16, (uint32_t)z);
    for (i = 0; i < 6; i++) put32(r + 20 + 4 * i, box + i);
    put32(r + 44, 6);
    tlv_put(b, RK_EV_SPRAY, r, sizeof r);
}

/* an RK_UNIT of slot s, its position (x, y, z) */
static void lo_at(RecBytes* b, unsigned s, int32_t x, int32_t y, int32_t z)
{
    static uint32_t img[NDW];
    int32_t v[3];
    v[0] = x;
    v[1] = y;
    v[2] = z;
    memset(img, 0, sizeof img);
    img[0] = 0x100 + s;
    memcpy((unsigned char*)img + REC_HOLD_POS_AT, v, 12);
    lo_unit(b, s, img);
}

static int cmp48(const void* a, const void* b) { return memcmp(a, b, 4 + REC_SPRAY_BODY); }

/* What a reader makes of the filter's iteration `out`, by the table tagpu_recreader.c keeps: its
   sprays taken as they come, its sessions as they come, and the implied ones made at its end,
   over the copy's units (the test's table holds no unit byte). 1 when those sprays are `in`'s as
   a multiset, 0 when not, -1 when the reader refuses the iteration; `implied` the ones it made. */
static int lo_read(RecSprays* t, const Exact* e, const RecBytes* in, const RecBytes* out,
                   unsigned* implied)
{
    unsigned char a[LO_SPRAYS][4 + REC_SPRAY_BODY], b[LO_SPRAYS][4 + REC_SPRAY_BODY];
    unsigned na = 0, nb = 0, i, n, T = get32(out->p + 4);
    const unsigned char* im;
    size_t at;
    *implied = 0;
    for (at = 0; at + 4 <= in->n; at += (4 + (size_t)get16(in->p + at + 2) + 3) & ~(size_t)3)
        if (get16(in->p + at) == RK_EV_SPRAY && na < LO_SPRAYS) memcpy(a[na++], in->p + at + 4, 48);
    for (at = 0; at + 4 <= out->n; at += (4 + (size_t)get16(out->p + at + 2) + 3) & ~(size_t)3) {
        unsigned kind = get16(out->p + at), len = get16(out->p + at + 2);
        if (kind == RK_SPRAY_SESSION && !recsprays_session(t, out->p + at + 4, len)) return -1;
        if (kind == RK_EV_SPRAY && nb < LO_SPRAYS) {
            memcpy(b[nb++], out->p + at + 4, 48);
            recsprays_spray(t, T, out->p + at + 4, e->units, e->live);
        }
    }
    if (recsprays_imply(t, T, e->units, e->live, &im, &n) != 1) return -1;
    *implied = n;
    for (i = 0; i < n && nb < LO_SPRAYS; i++) memcpy(b[nb++], im + 48 * i, 48);
    if (na != nb) return 0;
    qsort(a, na, sizeof a[0], cmp48);
    qsort(b, nb, sizeof b[0], cmp48);
    return !memcmp(a, b, (size_t)na * sizeof a[0]);
}

/* the session record `out` ends with, as its mode, or -1 for none */
static int lo_session_last(const RecBytes* out, unsigned* id)
{
    size_t at, last = (size_t)-1;
    for (at = 0; at + 4 <= out->n; at += (4 + (size_t)get16(out->p + at + 2) + 3) & ~(size_t)3)
        last = at;
    if (last == (size_t)-1 || get16(out->p + last) != RK_SPRAY_SESSION) return -1;
    *id = get32(out->p + last + 4);
    return out->p[last + 8];
}

/* an RK_SPRAY_SESSION's body */
static void lo_session(unsigned char* b, unsigned id, unsigned mode)
{
    memset(b, 0, 8);
    put32(b, id);
    b[4] = (unsigned char)mode;
}

static int outcomes_sprays(void)
{
    static unsigned char hold_unit[IMG], hold_node[REC_ORDER_IMAGE];
    unsigned char sb[8], live[LO_SLOTS];
    RecLossyParams lp;
    RecLossy* L;
    RecSprays* t;
    RecBytes in, out;
    Exact e;
    unsigned k[16], s[16], n, im, id = 0, ok = 1;
    int r;
    const unsigned char* imp;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    memset(&e, 0, sizeof e);
    lp.nslots = LO_SLOTS;
    lp.flags = RECL_SPRAYS;
    lp.hold_unit = hold_unit;
    lp.hold_node = hold_node;
    lp.gw = lp.gh = 0;
    L = reclossy_begin(&lp);
    t = recsprays_begin(LO_SLOTS);
    if (!L || !t || !exact_reset(&e, LO_SLOTS, 0, 0)) return 0;

    /* slot 1 (unit ID 2) sprays the same point; slot 2 (ID 3) moves and sprays with it */
    lo_tick(&in, 1);
    lo_at(&in, 1, 0, 0, 0);
    lo_at(&in, 2, 0, 0, 0);
    lo_spray(&in, 2, 100, 200, 300, 7);
    r = lo_snap(L, &e, &in, 1, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: a unit's first spray is written",
                 r == 1 && n == 4 && k[3] == RK_EV_SPRAY && lo_read(t, &e, &in, &out, &im) == 1, 1);
    lo_tick(&in, 2);
    lo_spray(&in, 2, 100, 200, 300, 7);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: a repeat is written, and opens a session of period 1",
                 r == 1 && n == 3 && k[1] == RK_EV_SPRAY && lo_session_last(&out, &id) == 1 &&
                 id == 2 && lo_read(t, &e, &in, &out, &im) == 1, 1);
    lo_tick(&in, 3);
    lo_spray(&in, 2, 100, 200, 300, 7);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: a spray implied plain: left out, and made by the reader",
                 r == 1 && n == 1 && lo_read(t, &e, &in, &out, &im) == 1 && im == 1, 1);
    lo_tick(&in, 4);
    lo_spray(&in, 3, 500, 0, 500, 9);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: a session closed in the iteration its unit makes no spray",
                 r == 1 && n == 3 && k[1] == RK_EV_SPRAY && lo_session_last(&out, &id) == 0 &&
                 id == 2 && lo_read(t, &e, &in, &out, &im) == 1 && im == 0, 1);
    lo_tick(&in, 5);
    lo_at(&in, 2, 10, 0, -20);
    lo_spray(&in, 3, 510, 0, 480, 9);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    ok &= expect("sprays: a spray moved with its unit opens a FOLLOW session",
                 r == 1 && lo_session_last(&out, &id) == (1 | REC_SPRAY_FOLLOW) && id == 3 &&
                 lo_read(t, &e, &in, &out, &im) == 1, 1);
    lo_tick(&in, 6);
    lo_at(&in, 2, 25, 1, -30);
    lo_spray(&in, 3, 525, 1, 470, 9);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: a spray implied FOLLOW: its point moved with its unit",
                 r == 1 && n == 2 && k[1] == RK_UNIT && lo_read(t, &e, &in, &out, &im) == 1 &&
                 im == 1, 1);
    lo_tick(&in, 7);
    lo_spray(&in, 3, 900, 0, 900, 9);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: explicit after a miss, its session kept",
                 r == 1 && n == 2 && k[1] == RK_EV_SPRAY && lo_read(t, &e, &in, &out, &im) == 1 &&
                 im == 0, 1);
    lo_tick(&in, 8);
    lo_spray(&in, 3, 900, 0, 900, 9);
    lo_spray(&in, 3, 900, 0, 900, 9);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: a unit's two sprays of one iteration are both written",
                 r == 1 && n == 3 && k[1] == RK_EV_SPRAY && k[2] == RK_EV_SPRAY &&
                 lo_read(t, &e, &in, &out, &im) == 1 && im == 0, 1);
    lo_tick(&in, 9);
    lo_spray(&in, 3, 900, 0, 900, 9);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("  and no session predicts from that iteration",
                 r == 1 && n == 2 && k[1] == RK_EV_SPRAY && lo_read(t, &e, &in, &out, &im) == 1, 1);
    lo_tick(&in, 10);
    lo_spray(&in, 3, 900, 0, 900, 9);
    r = lo_snap(L, &e, &in, 1, 1, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: a snapshot's iteration writes every spray and closes every session",
                 r == 1 && n == 3 && k[1] == RK_EV_SPRAY && lo_session_last(&out, &id) == 0 &&
                 id == 3 && lo_read(t, &e, &in, &out, &im) == 1 && im == 0, 1);
    recsprays_empty(t);             /* the reader passes the snapshot's block */
    lo_tick(&in, 11);
    lo_spray(&in, 3, 900, 0, 900, 9);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("  and empties the table: no session predicts from before it",
                 r == 1 && n == 2 && k[1] == RK_EV_SPRAY && lo_read(t, &e, &in, &out, &im) == 1, 1);
    lo_tick(&in, 12);
    lo_spray(&in, 0, 1, 2, 3, 4);
    lo_spray(&in, 0, 1, 2, 3, 4);
    r = lo_snap(L, &e, &in, 0, 0, &out);
    lo_tick(&in, 13);
    lo_spray(&in, 0, 1, 2, 3, 4);
    r = r == 1 && lo_snap(L, &e, &in, 0, 0, &out) == 1;
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("sprays: a spray that names no unit is written and opens no session",
                 r && n == 2 && k[1] == RK_EV_SPRAY, 1);
    lo_tick(&in, 14);
    lo_session(sb, 2, 1);
    tlv_put(&in, RK_SPRAY_SESSION, sb, 8);
    ok &= expect("sprays: refused: a session record among the capture's",
                 lo_snap(L, &e, &in, 0, 0, &out), 0);

    /* the readers' refusals: what the filter never writes */
    recsprays_empty(t);
    memcpy(live, e.live, LO_SLOTS);
    lo_session(sb, 2, 1);
    ok &= expect("sprays: refused: a session predicting from no spray",
                 recsprays_session(t, sb, 8) && recsprays_imply(t, 20, e.units, live, &imp, &n) == 0, 1);
    recsprays_empty(t);
    in.n = 0;
    lo_spray(&in, 3, 1, 2, 3, 4);
    recsprays_spray(t, 20, in.p + 4, e.units, live);
    recsprays_spray(t, 20, in.p + 4, e.units, live);
    ok &= expect("sprays: refused: a session predicting from two sprays",
                 recsprays_session(t, (lo_session(sb, 3, 1), sb), 8) &&
                 recsprays_imply(t, 21, e.units, live, &imp, &n) == 0, 1);
    recsprays_empty(t);
    recsprays_spray(t, 20, in.p + 4, e.units, live);
    live[2] = 0;
    ok &= expect("sprays: refused: a FOLLOW session whose unit died",
                 recsprays_session(t, (lo_session(sb, 3, 1 | REC_SPRAY_FOLLOW), sb), 8) &&
                 recsprays_imply(t, 21, e.units, live, &imp, &n) == 0, 1);
    live[2] = 1;
    recsprays_empty(t);
    recsprays_spray(t, 20, in.p + 4, e.units, live);
    ok &= expect("sprays: a session of period 2 predicts from two GameTimes before",
                 recsprays_session(t, (lo_session(sb, 3, 2), sb), 8) &&
                 recsprays_imply(t, 21, e.units, live, &imp, &n) == 0, 1);
    recsprays_empty(t);
    recsprays_spray(t, 20, in.p + 4, e.units, live);
    ok &= expect("  and makes its spray there",
                 recsprays_session(t, (lo_session(sb, 3, 2), sb), 8) &&
                 recsprays_imply(t, 22, e.units, live, &imp, &n) == 1 && n == 1 &&
                 !memcmp(imp, in.p + 4, 48), 1);
    {
        static const unsigned bad[][2] = { { 1, 1 }, { LO_SLOTS + 1, 1 }, { 0, 1 }, { 2, 3 },
                                           { 2, REC_SPRAY_FOLLOW }, { 2, 0x11 } };
        unsigned i, refused = 1;
        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            lo_session(sb, bad[i][0], bad[i][1]);
            refused &= !recsprays_session(t, sb, 8);
        }
        lo_session(sb, 2, 1);
        sb[6] = 1;
        refused &= !recsprays_session(t, sb, 8);
        sb[6] = 0;
        refused &= !recsprays_session(t, sb, 4);
        ok &= expect("sprays: refused: a session naming no unit slot, a mode outside 0 1 2 4 "
                     "(+FOLLOW), a byte past it not zero, a length not 8", refused, 1);
    }
    recsprays_free(t);
    reclossy_free(L);
    exact_free(&e);
    rb_free(&in);
    rb_free(&out);
    return ok;
}

/* a run of n cell images of the grid's row z from column x, one record */
static void lo_cells(RecBytes* b, unsigned x, unsigned z, const unsigned char* cells, unsigned n)
{
    unsigned char r[8 + 4 * REC_CELL_BYTES];
    memset(r, 0, 8);
    r[0] = (unsigned char)x;
    r[2] = (unsigned char)z;
    r[4] = (unsigned char)n;
    memcpy(r + 8, cells, (size_t)n * REC_CELL_BYTES);
    tlv_put(b, RK_CELLS, r, 8 + n * REC_CELL_BYTES);
}

/* THE CONTEST'S RULE's bookkeeping (tagpu_contest.h) on sites of the test's own: what each
   holds, what a change of rule writes, and when a network game is refused */
static int outcomes_contest(void)
{
    static const unsigned char je[2] = { 0x74, 0x25 }, other[2] = { 0xEB, 0x25 };
    int site[CONTEST_SITES], refuse, ok = 1;
    unsigned i, plan;
    ok &= expect("contest: the retail je", contest_classify(je, je, 0), SITE_RETAIL);
    ok &= expect("contest: the retail je, the record stale", contest_classify(je, je, 1),
                 SITE_RETAIL);
    ok &= expect("contest: NOPs the record claims", contest_classify(CONTEST_NOP2, je, 1),
                 SITE_OURS);
    ok &= expect("contest: NOPs it does not claim are another's",
                 contest_classify(CONTEST_NOP2, je, 0), SITE_FOREIGN);
    ok &= expect("contest: other bytes are another's, claimed or not",
                 contest_classify(other, je, 1), SITE_FOREIGN);
    for (i = 0; i < CONTEST_SITES; i++) site[i] = SITE_RETAIL;
    plan = contest_plan(1, site, &refuse);
    ok &= expect("contest: the rule goes into six retail sites", (int)plan, 0x3F);
    ok &= expect("  and is not refused", refuse, 0);
    ok &= expect("contest: putting back with none of ours writes nothing",
                 (int)contest_plan(0, site, &refuse), 0);
    site[2] = SITE_FOREIGN;
    ok &= expect("contest: another's bytes refuse the rule, writing nothing",
                 (int)contest_plan(1, site, &refuse), 0);
    ok &= expect("  refused", refuse, 1);
    ok &= expect("  and are left by a restore", (int)contest_plan(0, site, &refuse), 0);
    ok &= expect("  which is not refused", refuse, 0);
    ok &= expect("  and do not refuse a network game", contest_any(site, SITE_OURS), 0);
    site[0] = site[4] = SITE_OURS;
    ok &= expect("contest: a restore writes ours alone", (int)contest_plan(0, site, &refuse),
                 0x11);
    ok &= expect("  and ours refuse a network game", contest_any(site, SITE_OURS), 1);
    site[2] = SITE_RETAIL;
    ok &= expect("contest: the rule completes from ours and retail sites",
                 (int)contest_plan(1, site, &refuse), 0x2E);
    return ok;
}

/* RECL_CELLS: the cells' unit slots (+0x00, +0x02) and yard bit (+0x0C bit 1) held */
static int outcomes_lossy_cells(void)
{
    enum { GW = 4, GH = 2, CB = REC_CELL_BYTES };
    static const unsigned char none[REC_UNIT_IMAGE + REC_ORDER_IMAGE];
    unsigned char row[2][GW * CB], run[2 * CB];
    RecLossyParams lp;
    RecLossy* L;
    RecBytes in, out;
    Exact e;
    unsigned k[16], s[16], n, i, ok = 1;
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    memset(&e, 0, sizeof e);
    for (i = 0; i < GW * CB; i++) {
        row[0][i] = (unsigned char)(0x10 + i);
        row[1][i] = (unsigned char)(0x80 + i);
    }
    lp.nslots = LO_SLOTS;
    lp.hold_unit = none;
    lp.hold_node = none + REC_UNIT_IMAGE;
    lp.gw = GW;
    lp.gh = GH;
    lp.flags = 4;
    ok &= expect("lossy cells: refused: a flag the filter does not keep", reclossy_begin(&lp) == NULL, 1);
    lp.flags = RECL_CELLS;
    L = reclossy_begin(&lp);
    if (!L || !exact_reset(&e, LO_SLOTS, GW, GH)) return 0;
    lo_tick(&in, 1);
    lo_cells(&in, 0, 0, row[0], GW);
    lo_cells(&in, 0, 1, row[1], GW);
    lo_event(&in);
    ok &= expect("lossy cells: the first iteration copied",
                 lo_step(L, &e, &in, 1, &out) == 1 && out.n == in.n && !memcmp(out.p, in.p, in.n), 1);
    {
        RecLossyExact x = { e.units, e.live, e.lists, e.grid };
        ok &= expect("  the lossy grid is the copy's", reclossy_equal(L, &x), 1);
    }
    /* cell (1, 0) changes its slots alone, cell (2, 0) its slot A and its byte 5 */
    memcpy(run, row[0] + CB, 2 * CB);
    run[0] ^= 1;
    run[CB + 0] ^= 2;
    run[CB + 5] ^= 4;
    lo_tick(&in, 2);
    lo_cells(&in, 1, 0, run, 2);
    lo_event(&in);
    ok &= expect("lossy cells: an image between refreshes", lo_step(L, &e, &in, 0, &out), 1);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("  the run is written, its slots D's and its other bytes the input's",
                 n == 3 && k[1] == RK_CELLS && out.n >= 8 + 4 + 8 + 2 * CB &&
                 !memcmp(out.p + 8 + 12, row[0] + CB, CB) &&
                 !memcmp(out.p + 8 + 12 + CB, row[0] + 2 * CB, 5) &&
                 out.p[8 + 12 + CB + 5] == (unsigned char)(row[0][2 * CB + 5] ^ 4) &&
                 !memcmp(out.p + 8 + 12 + CB + 6, row[0] + 2 * CB + 6, CB - 6), 1);
    {
        RecLossyExact x = { e.units, e.live, e.lists, e.grid };
        ok &= expect("  the lossy grid differs from the copy's", reclossy_equal(L, &x), 0);
    }
    /* cell (3, 0) changes its yard bit alone: nothing to write */
    memcpy(run, row[0] + 3 * CB, CB);
    run[CB - 1] ^= 0x02;
    lo_tick(&in, 3);
    lo_cells(&in, 3, 0, run, 1);
    lo_event(&in);
    ok &= expect("lossy cells: an image of a yard bit alone", lo_step(L, &e, &in, 0, &out), 1);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("  is dropped", n == 2 && k[0] == RK_TICK && k[1] == RK_EV_STATE, 1);
    /* the tick's input, after an event: a run that differs from D's in the slots and the yard
       bit is dropped, one that differs in another byte is written as it came */
    memcpy(run, row[1], CB);
    run[1] ^= 1;
    run[CB - 1] ^= 0x02;
    lo_tick(&in, 4);
    lo_event(&in);
    lo_cells(&in, 0, 1, run, 1);
    run[4] ^= 1;
    lo_cells(&in, 0, 1, run, 1);
    ok &= expect("lossy cells: the tick's input", lo_step(L, &e, &in, 0, &out), 1);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("  a run of slots and a yard bit dropped, another kept whole",
                 n == 3 && k[1] == RK_EV_STATE && k[2] == RK_CELLS &&
                 out.n == 8 + 12 + ((4 + 8 + CB + 3) & ~3u) &&
                 !memcmp(out.p + 8 + 12 + 12, run, CB), 1);
    lo_tick(&in, 5);
    lo_event(&in);
    ok &= expect("lossy cells: the refresh after them", lo_step(L, &e, &in, 1, &out), 1);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("  writes the cells that differ, a run of a row, before the event",
                 n == 3 && k[1] == RK_CELLS && get16(out.p + 12) == 1 && get16(out.p + 14) == 0 &&
                 get16(out.p + 16) == 3 && k[2] == RK_EV_STATE, 1);
    {
        RecLossyExact x = { e.units, e.live, e.lists, e.grid };
        ok &= expect("  the lossy grid is the copy's", reclossy_equal(L, &x), 1);
        lo_tick(&in, 6);
        lo_cells(&in, 3, 1, row[1], 2);
        ok &= expect("lossy cells: refused: a run past the row",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
        lo_tick(&in, 6);
        lo_cells(&in, 0, GH, row[1], 1);
        ok &= expect("lossy cells: refused: a row past the grid",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
    }
    reclossy_free(L);
    exact_free(&e);
    rb_free(&in);
    rb_free(&out);
    return ok;
}

static int outcomes_lossy(void)
{
    static uint32_t img[3][NDW];
    static unsigned char hold_unit[IMG], hold_node[REC_ORDER_IMAGE], node[2][REC_ORDER_IMAGE];
    unsigned char hold[REC_HOLD_BYTES];
    RecLossyParams lp;
    RecLossy* L;
    RecLossyExact x;
    RecLossyHold hh;
    RecBytes in, out;
    Exact e;
    unsigned k[16], s[16], n, i, ok = 1;
    char why[160];
    memset(&in, 0, sizeof in);
    memset(&out, 0, sizeof out);
    memset(&e, 0, sizeof e);
    for (i = 0; i < NDW; i++) {
        img[1][i] = 0x1000 + i;
        img[2][i] = 0x2000 + i;
    }
    memset(node, 0x11, sizeof node);

    /* an empty table: every iteration's records come out as they went in */
    lp.nslots = LO_SLOTS;
    lp.flags = 0;
    lp.hold_unit = hold_unit;
    lp.hold_node = hold_node;
    lp.gw = lp.gh = 0;
    L = reclossy_begin(&lp);
    if (!L || !exact_reset(&e, LO_SLOTS, 0, 0)) return 0;
    lo_tick(&in, 1);
    lo_unit(&in, 1, img[1]);
    lo_unit(&in, 2, img[2]);
    lo_list(&in, 1, 0, node[0], 1, 0, 1);
    lo_event(&in);
    ok &= expect("lossy: a table of no rows copies a refresh iteration",
                 lo_step(L, &e, &in, 1, &out) == 1 && out.n == in.n && !memcmp(out.p, in.p, in.n), 1);
    {
        unsigned d[2] = { LO_HELD, 3 };
        uint32_t v[2] = { 0xAAAA, 0xBBBB };
        unsigned char moved[REC_ORDER_IMAGE];
        memcpy(moved, node[0], sizeof moved);
        moved[4 * LO_NODE] ^= 1;
        lo_tick(&in, 2);
        lo_delta(&in, 1, d, v, 2);
        lo_list(&in, 1, 0, moved, 1, 0, 1);
        lo_event(&in);
        ok &= expect("lossy: ... and the next, between refreshes",
                     lo_step(L, &e, &in, 0, &out) == 1 && out.n == in.n && !memcmp(out.p, in.p, in.n), 1);
    }
    reclossy_free(L);

    /* a table that holds dword LO_HELD, LO_PART's bits 0xFF00 and a main node's dword LO_NODE */
    memset(hold_unit + 4 * LO_HELD, 0xFF, 4);
    hold_unit[4 * LO_PART + 1] = 0xFF;
    memset(hold_node + 4 * LO_NODE, 0xFF, 4);
    L = reclossy_begin(&lp);
    if (!L || !exact_reset(&e, LO_SLOTS, 0, 0)) return 0;
    lo_tick(&in, 1);
    lo_unit(&in, 1, img[1]);
    lo_unit(&in, 2, img[2]);
    lo_list(&in, 1, 0, node[0], 1, 0, 1);
    ok &= expect("lossy: held: the first iteration copied",
                 lo_step(L, &e, &in, 1, &out) == 1 && out.n == in.n && !memcmp(out.p, in.p, in.n), 1);
    {
        unsigned d1[1] = { LO_HELD }, d2[2] = { 10, LO_PART };
        uint32_t v1[1] = { 0xDEAD }, v2[2] = { 0x77, 0x00123456 };
        unsigned char moved[REC_ORDER_IMAGE];
        memcpy(moved, node[0], sizeof moved);
        moved[4 * LO_NODE] ^= 1;
        lo_tick(&in, 2);
        lo_delta(&in, 1, d1, v1, 1);
        lo_delta(&in, 2, d2, v2, 2);
        lo_list(&in, 1, 0, moved, 1, 0, 1);
        lo_event(&in);
        ok &= expect("lossy: held: an iteration between refreshes", lo_step(L, &e, &in, 0, &out), 1);
        n = lo_kinds(&out, k, s, 16);
        ok &= expect("  a delta of held bits alone and a list of held bytes alone are dropped",
                     n == 3 && k[0] == RK_TICK && k[1] == RK_UNIT_DELTA && s[1] == 2 &&
                     k[2] == RK_EV_STATE, 1);
        ok &= expect("  a dword of shown and held bits keeps the held ones",
                     out.n >= 8 + 4 + REC_UNIT_DELTA_HEAD + 8 &&
                     get32(out.p + 8 + 4 + REC_UNIT_DELTA_HEAD) == 0x77 &&
                     get32(out.p + 8 + 4 + REC_UNIT_DELTA_HEAD + 4) ==
                         ((img[2][LO_PART] & 0xFF00) | (0x00123456 & ~0xFF00u)), 1);
        x.units = e.units;
        x.live = e.live;
        x.lists = e.lists;
        x.grid = e.grid;
        ok &= expect("  the lossy world differs from the copy", reclossy_equal(L, &x), 0);
    }
    lo_tick(&in, 3);
    lo_event(&in);
    ok &= expect("lossy: held: the refresh after it", lo_step(L, &e, &in, 1, &out), 1);
    n = lo_kinds(&out, k, s, 16);
    ok &= expect("  writes exactly the differences, the images before the event",
                 n == 5 && k[1] == RK_UNIT_DELTA && s[1] == 1 && k[2] == RK_UNIT_DELTA && s[2] == 2 &&
                 k[3] == RK_ORDERS && s[3] == 1 && k[4] == RK_EV_STATE, 1);
    {
        /* each delta names its one dword, at the copy's value */
        size_t a1 = 8, a2 = a1 + ((4 + get16(out.p + a1 + 2) + 3) & ~3u);
        ok &= expect("  each delta one dword, the copy's",
                     get16(out.p + a1 + 2) == REC_UNIT_DELTA_HEAD + 4 &&
                     get32(out.p + a1 + 4 + REC_UNIT_DELTA_HEAD) == 0xDEAD &&
                     get16(out.p + a2 + 2) == REC_UNIT_DELTA_HEAD + 4 &&
                     get32(out.p + a2 + 4 + REC_UNIT_DELTA_HEAD) == 0x00123456, 1);
    }
    x.units = e.units;
    x.live = e.live;
    x.lists = e.lists;
    x.grid = e.grid;
    ok &= expect("  the lossy world is the copy", reclossy_equal(L, &x), 1);
    e.units[IMG + 0x40] ^= 1;
    ok &= expect("lossy: the check fails on one byte of a unit", reclossy_equal(L, &x), 0);
    e.units[IMG + 0x40] ^= 1;
    e.lists[2].img[5] ^= 1;
    ok &= expect("lossy: ... and on one byte of a list", reclossy_equal(L, &x), 0);
    e.lists[2].img[5] ^= 1;
    e.live[5] = 1;
    ok &= expect("lossy: ... and on a slot's liveness", reclossy_equal(L, &x), 0);
    e.live[5] = 0;

    /* malformed iterations are refused */
    {
        unsigned d[1] = { NDW };
        uint32_t v[1] = { 1 };
        lo_tick(&in, 4);
        lo_list(&in, 1, 2, node[0], 1, 0, 1);
        ok &= expect("lossy: refused: a list that is neither main nor sub",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
        lo_tick(&in, 4);
        lo_list(&in, 1, 0, node[0], 1, 0, 2);
        ok &= expect("lossy: refused: a list unfinished at the iteration's end",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
        lo_tick(&in, 4);
        lo_list(&in, 1, 0, node[0], 1, 1, 2);
        ok &= expect("lossy: refused: a list's continuation with no beginning",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
        lo_tick(&in, 4);
        lo_list(&in, 3, 0, node[0], 1, 0, 1);
        ok &= expect("lossy: refused: a list of a slot not live",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
        lo_tick(&in, 4);
        lo_delta(&in, 1, d, v, 1);
        ok &= expect("lossy: refused: a delta naming a dword past the image",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
        lo_tick(&in, 4);
        lo_unit(&in, LO_SLOTS, img[1]);
        ok &= expect("lossy: refused: a slot past the header's",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
        {
            /* a slot the first pass marked before it refused is not left marked: the refresh
               after it still brings that slot's held dword to the copy */
            unsigned dh[1] = { LO_HELD };
            uint32_t vh[1] = { 0xBEEF };
            lo_tick(&in, 4);
            lo_delta(&in, 1, dh, vh, 1);
            lo_unit(&in, LO_SLOTS, img[1]);
            ok &= expect("lossy: refused: a slot past the header's after a valid one",
                         reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
            lo_tick(&in, 5);
            lo_delta(&in, 1, dh, vh, 1);
            ok &= expect("  a held change after it", lo_step(L, &e, &in, 0, &out), 1);
            lo_tick(&in, 6);
            lo_event(&in);
            ok &= expect("  the refresh after it", lo_step(L, &e, &in, 1, &out), 1);
            x.units = e.units;
            x.live = e.live;
            x.lists = e.lists;
            x.grid = e.grid;
            ok &= expect("  the lossy world is the copy", reclossy_equal(L, &x), 1);
        }
        in.n = 0;
        lo_event(&in);
        ok &= expect("lossy: refused: an iteration that does not begin with RK_TICK",
                     reclossy_iteration(L, in.p, in.n, 0, 0, &x, &out), 0);
    }
    reclossy_free(L);
    ok &= outcomes_lossy_cells();
    ok &= outcomes_contest();

    /* RT_HOLD's bounds */
    hold_bytes(hold, 1);
    {
        unsigned char sha[32];
        unsigned i, x, same = 1;
        for (i = 0; i < 32; i++) {
            same &= sscanf(TAGPU_REPLAY_HOLD_SHA256 + 2 * i, "%2x", &x) == 1;
            sha[i] = (unsigned char)x;
        }
        ok &= expect("RT_HOLD: the build's table reads back as the build's masks and SHA-256",
                     reclossy_hold_parse(&hh, hold, sizeof hold, why, sizeof why) && same &&
                         hh.refresh == REC_REFRESH_TICKS && hh.flags == REC_SKIRMISH_HOLDS &&
                         !memcmp(hh.unit, REC_HOLD_UNIT, REC_UNIT_IMAGE) &&
                         !memcmp(hh.node, REC_HOLD_NODE, REC_ORDER_IMAGE) &&
                         !memcmp(hh.sha, sha, 32), 1);
    }
    hold_bytes(hold, 0);
    ok &= expect("RT_HOLD: a table of no rows reads", reclossy_hold_parse(&hh, hold, sizeof hold, why, sizeof why), 1);
    ok &= expect("RT_HOLD: refused: another length", reclossy_hold_parse(&hh, hold, sizeof hold - 4, why, sizeof why), 0);
    hold[40 + 0x70] = 1;
    ok &= expect("RT_HOLD: refused: a held bit of the position +0x6A..+0x75",
                 reclossy_hold_parse(&hh, hold, sizeof hold, why, sizeof why), 0);
    hold[40 + 0x70] = 0;
    hold[40 + 0x76] = 1;
    ok &= expect("RT_HOLD: a held bit past the position reads", reclossy_hold_parse(&hh, hold, sizeof hold, why, sizeof why), 1);
    hold[4] = 4;
    ok &= expect("RT_HOLD: refused: a flag outside RECL_CELLS | RECL_SPRAYS",
                 reclossy_hold_parse(&hh, hold, sizeof hold, why, sizeof why), 0);
    hold[4] = RECL_CELLS | RECL_SPRAYS;
    ok &= expect("RT_HOLD: both flags read", reclossy_hold_parse(&hh, hold, sizeof hold, why, sizeof why), 1);
    put32(hold, REC_REFRESH_MIN - 1);
    ok &= expect("RT_HOLD: refused: a refresh interval under REC_REFRESH_MIN",
                 reclossy_hold_parse(&hh, hold, sizeof hold, why, sizeof why), 0);
    put32(hold, REC_REFRESH_TICKS + 1);
    ok &= expect("RT_HOLD: refused: a refresh interval over REC_REFRESH_TICKS",
                 reclossy_hold_parse(&hh, hold, sizeof hold, why, sizeof why), 0);

    /* the writer's grid: the first two iterations, then 1 modulo the interval, or the first
       iteration past one a gap skips */
    {
        static const unsigned run[9] = { 1, 2, 3, 30, 31, 32, 60, 62, 91 };
        static const unsigned want[9] = { 1, 1, 0, 0, 1, 0, 0, 1, 1 };
        unsigned i, prev = 0, same = 1;
        for (i = 0; i < 9; i++) {
            int r = reclossy_is_refresh(i + 1, run[i], prev, 30);
            same &= (unsigned)r == want[i];
            if (r) prev = run[i];
        }
        ok &= expect("is_refresh: the first two, 1 modulo 30, past a gap", same, 1);
        ok &= expect("is_refresh: a segment's second iteration off the grid",
                     reclossy_is_refresh(2, 501, 500, 30), 1);
        ok &= expect("is_refresh: its third", reclossy_is_refresh(3, 502, 501, 30), 0);
    }
    /* the refresh at or before T, from a segment's stream blocks as the writer cuts them: its
       first two iterations each a refresh block of its own */
    {
        static const RecLossyBlock root[7] = { { 1, 1, 1 }, { 2, 2, 1 }, { 3, 31, 0 },
                                               { 32, 300, 0 }, { 301, 301, 1 }, { 302, 331, 0 },
                                               { 332, 601, 1 } };
        static const RecLossyBlock fork[4] = { { 451, 451, 1 }, { 452, 452, 1 }, { 453, 481, 0 },
                                               { 482, 601, 1 } };
        ok &= expect("refresh_at: before the segment, none", reclossy_refresh_at(root, 7, 1, 0), 0);
        ok &= expect("refresh_at: the first iteration", reclossy_refresh_at(root, 7, 1, 1), 1);
        ok &= expect("refresh_at: the first interval, the second iteration",
                     reclossy_refresh_at(root, 7, 1, 150), 2);
        ok &= expect("refresh_at: a grid point", reclossy_refresh_at(root, 7, 1, 301), 301);
        ok &= expect("refresh_at: the iteration before the next", reclossy_refresh_at(root, 7, 1, 600), 301);
        ok &= expect("refresh_at: past the last block", reclossy_refresh_at(root, 7, 1, 10000), 601);
        ok &= expect("refresh_at: a fork's, before it", reclossy_refresh_at(fork, 4, 451, 450), 0);
        ok &= expect("refresh_at: a fork's first iteration", reclossy_refresh_at(fork, 4, 451, 451), 451);
        ok &= expect("refresh_at: a fork's first interval, its second iteration",
                     reclossy_refresh_at(fork, 4, 451, 500), 452);
        ok &= expect("refresh_at: a fork's grid point", reclossy_refresh_at(fork, 4, 451, 601), 601);
    }
    exact_free(&e);
    rb_free(&in);
    rb_free(&out);
    return ok;
}

static int cmd_outcomes(void)
{
    static uint32_t units[6][NDW];
    static PSim q;
    unsigned seen[5] = { 0, 0, 0, 0, 0 };
    RecBytes y;
    RecCodecParams p;
    RecStream st;
    RecCodec* rd;
    World w;
    unsigned char codec[4 + 4 * REC_LANES_N], phys[8], air[8 + 24];
    unsigned j, flags, block, ok = 1;
    uint32_t gt = 1;
    const unsigned char* body;
    size_t bn;
    memset(&w, 0, sizeof w);
    memset(&y, 0, sizeof y);
    crc_init();
    codec[0] = REC_LANES_N;
    codec[1] = codec[2] = codec[3] = 0;
    for (j = 0; j < REC_LANES_N; j++) {
        codec[4 + 4 * j] = (unsigned char)rec_lanes[j].off;
        codec[5 + 4 * j] = (unsigned char)(rec_lanes[j].off >> 8);
        codec[6 + 4 * j] = rec_lanes[j].width;
        codec[7 + 4 * j] = rec_lanes[j].role;
    }
    put32(phys, 8155);
    phys[4] = 75;
    phys[5] = 1;
    phys[6] = phys[7] = 0;
    memset(air, 0, sizeof air);
    put32(air, 1);
    air[8] = 51;
    air[10] = 10;
    put32(air + 12, 10 << 16);
    put32(air + 16, 6 << 16);
    put32(air + 20, 22937);
    put32(air + 24, 1 << 16);
    if (!reccodec_params(&p, 64, PJ_N, 16, 16, 64, codec, sizeof codec, phys, 8, air, sizeof air, NULL, 0) ||
        !reccodec_stream_begin(&st, &p) || !(rd = reccodec_new(&p)))
        return 2;
    world_reset(&w, 64, PJ_N, 16, 16);
    q.first = 1;
    for (block = 0; block < 22; block++) {
        static const int want[22] = { RECC_SECTIONS, RECC_SECTIONS, RECC_DECLINED, RECC_SECTIONS,
                                      RECC_DECLINED, RECC_SECTIONS, RECC_MISMATCH, RECC_SECTIONS,
                                      RECC_DECLINED, RECC_SECTIONS, RECC_DECLINED, RECC_SECTIONS,
                                      RECC_DECLINED, RECC_SECTIONS, RECC_DECLINED, RECC_SECTIONS,
                                      RECC_DECLINED, RECC_SECTIONS, RECC_DECLINED, RECC_SECTIONS,
                                      RECC_MISMATCH, RECC_SECTIONS };
        static const int pfault[22] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, PJ_TWICE, 0, PJ_HEADS, 0,
                                        PJ_SHRINK, 0, PJ_UNCHANGED, 0, PJ_PAD, 0, 0, 0 };
        static const char* what[22] = {
            "SECTIONS, the segment's first block", "SECTIONS, continuing",
            "DECLINED: a delta naming an unchanged dword", "SECTIONS, restarting after it",
            "DECLINED: a correction on a delta's slot", "SECTIONS, restarting after it",
            "MISMATCH: the check decoder restarted from other units", "SECTIONS, restarting after it",
            "DECLINED: the encoder's lists failed an allocation",
            "SECTIONS, restarting after it (the reader's list failed too)",
            "DECLINED: a correction naming a projectile slot again", "SECTIONS, restarting after it",
            "DECLINED: two RK_PROJ_HEAD", "SECTIONS, restarting after it",
            "DECLINED: an RK_PROJ_HEAD that shrinks the extent", "SECTIONS, restarting after it",
            "DECLINED: a projectile delta naming an unchanged dword", "SECTIONS, restarting after it",
            "DECLINED: a projectile delta whose pad is not zero", "SECTIONS, restarting after it",
            "MISMATCH: the check decoder restarted from another pool", "SECTIONS, restarting after it" };
        char line[128];
        unsigned before[5] = { q.births, q.deaths, q.guided, q.tail, q.grows }, k;
        int o;
        /* block 9 creates its units again, so the reader's touched list is used after its failure */
        synth_block(&y, units, &q, &gt, block == 0 || block == 9, block == 2, block == 4, pfault[block]);
        if (block == 8) {
            /* a failed realloc, as rb_grow latches it: a restart must clear it on both sides */
            st.enc->dl.oom = 1;
            st.enc->hash.oom = 1;
            rd->tl.oom = 1;
        }
        if (block == 6) {
            static uint32_t other[64][NDW];
            static unsigned char live[64];
            memcpy(other, reccodec_units(st.enc), sizeof other);
            memcpy(live, reccodec_live(st.enc), sizeof live);
            other[2][20] ^= 0x10;
            {
                unsigned count, hw;
                const unsigned char* pool = reccodec_pool(st.enc, &count, &hw);
                RecFrameWorld fw;
                reccodec_world(st.enc, &fw);
                reccodec_restart(st.enc, &st.p, reccodec_units(st.enc), reccodec_live(st.enc), pool, count, hw, &fw);
                reccodec_restart(st.chk, &st.p, &other[0][0], live, pool, count, hw, &fw);
            }
            st.reset = 0;
        }
        if (block == 20) {
            /* every live record's x one unit off: each survivor's prediction differs */
            static unsigned char other[PJ_N * PIMG];
            unsigned count, hw, i;
            RecFrameWorld fw;
            reccodec_world(st.enc, &fw);
            memcpy(other, reccodec_pool(st.enc, &count, &hw), sizeof other);
            for (i = 0; i < count; i++) other[(size_t)i * PIMG + 4] ^= 1;
            ok &= expect("  the pool has a live record to differ in", count > 0, 1);
            reccodec_restart(st.enc, &st.p, reccodec_units(st.enc), reccodec_live(st.enc),
                             reccodec_pool(st.enc, &count, &hw), count, hw, &fw);
            reccodec_restart(st.chk, &st.p, reccodec_units(st.enc), reccodec_live(st.enc), other, count, hw, &fw);
            st.reset = 0;
        }
        o = reccodec_stream_block(&st, y.p, y.n, &flags, &body, &bn);
        snprintf(line, sizeof line, "block %u: %s", block, what[block]);
        ok &= expect(line, o, want[block]);
        if (o == RECC_SECTIONS)
            ok &= expect("  flagged RBF_FRAME_RESET exactly when it restarts", !!(flags & RBF_FRAME_RESET),
                         block != 1);
        ok &= expect("  a reader decodes it back", reader_check(rd, &w, &p, flags, body, bn,
                                                                  o == RECC_SECTIONS ? st.sec.n : y.n,
                                                                  y.p, y.n, st.dctx), 1);
        ok &= expect("  the encoder's copy is the payload's end state", world_same(&w, st.enc), 1);
        if (o == RECC_SECTIONS) {
            unsigned after[5] = { q.births, q.deaths, q.guided, q.tail, q.grows };
            for (k = 0; k < 5; k++) seen[k] += after[k] - before[k];
        }
    }
    printf("the sectioned blocks carried %u births from shots, %u deaths, %u guided steps, %u changed "
           "stale slots and %u extent growths\n", seen[0], seen[1], seen[2], seen[3], seen[4]);
    ok &= expect("projectiles: births, deaths, guided survivors, a changed tail, a growing extent",
                 seen[0] && seen[1] && seen[2] && seen[3] && seen[4], 1);
    ok &= outcomes_cells(&p);
    ok &= outcomes_orders(&p);
    ok &= outcomes_events(&p);
    ok &= outcomes_state(&p);
    ok &= outcomes_lossy();
    ok &= outcomes_sprays();
    ok &= zstd_window_check(st.cctx, st.dctx);
    ok &= f32_check();
    printf("outcomes: %u sectioned, %u declined, %u mismatched\n", st.sectioned, st.declined, st.mismatched);
    reccodec_stream_end(&st);
    reccodec_free(rd);
    rb_free(&y);
    world_free(&w);
    return ok ? 0 : 1;
}

/* ---- fuzz ------------------------------------------------------------------------------------- */

static uint64_t rng_state;
static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * UINT64_C(2685821657736338717);
}
static size_t rndn(size_t n) { return n ? (size_t)(rnd() % n) : 0; }

/* A copy of a decoder's state: every field reccodec_decode carries from one block to the next
   (the units' copy and history, the ledger's steps and settles, the pool, the record frames).
   Every case reuses one decoder, so a field missed here starts a case from the previous case's
   leftovers; cmd_fuzz's control names the first block where that shows. */
static void codec_copy(RecCodec* d, const RecCodec* s)
{
    size_t n = s->nslots;
    d->valid = s->valid;
    d->p = s->p;
    memcpy(d->lane_of, s->lane_of, sizeof d->lane_of);
    memcpy(d->s1, s->s1, sizeof d->s1);
    memcpy(d->s2, s->s2, sizeof d->s2);
    memcpy(d->cnt, s->cnt, sizeof d->cnt);
    d->ns1 = s->ns1;
    d->ns2 = s->ns2;
    d->ncnt = s->ncnt;
    memcpy(d->defwin, s->defwin, sizeof d->defwin);
    d->q = s->q;
    memcpy(d->airix, s->airix, 65536 * sizeof *d->airix);
    memcpy(d->unit, s->unit, n * IMG);
    memcpy(d->l2, s->l2, n * IMG);
    memcpy(d->l3, s->l3, n * L3B);
    memcpy(d->win, s->win, n * RECC_LANES_MAX);
    memcpy(d->age, s->age, n);
    memcpy(d->rest, s->rest, n);
    memcpy(d->live, s->live, n);
    memcpy(d->touched, s->touched, n);
    memcpy(d->lstep, s->lstep, n * 2 * sizeof *d->lstep);
    memcpy(d->lsettle, s->lsettle, sizeof d->lsettle);
    memcpy(d->lsettled, s->lsettled, sizeof d->lsettled);
    n = s->nproj;
    d->pcount = s->pcount;
    d->phw = s->phw;
    d->nshots = s->nshots;
    memcpy(d->pool, s->pool, n * PIMG);
    memcpy(d->pool_n, s->pool_n, n * PIMG);
    memcpy(d->pool_l2, s->pool_l2, n * PIMG);
    memcpy(d->pwin, s->pwin, n * PDW);
    memcpy(d->page, s->page, n);
    memcpy(d->lastnew, s->lastnew, (size_t)(RECC_WEAPON_IDS + 1) * PIMG);
    memcpy(d->shots, s->shots, (size_t)RECC_SHOTS_MAX * SHOT);
    for (n = 0; n < RECC_FRAMES; n++)
        if (!FRAMES[n]->copy(d->frame[n], s->frame[n])) {
            fprintf(stderr, "codectest: no memory to copy the %s frame\n", FRAMES[n]->name);
            exit(2);
        }
}

/* the varint at `at` and its end; 0 when it does not read */
static size_t skip_varint(const unsigned char* p, size_t end, size_t at)
{
    uint32_t v;
    return varint_get(p, end, &at, &v) ? at : 0;
}

/* RCS_UNITS's regions: [0] the prefix (iterations, listed, gaps, ndict), [1] the masks, [2] the
   ids, [3] the columns, as offsets into the container */
static void unit_regions(const unsigned char* sec, size_t a, size_t e, unsigned nlanes, size_t r[5])
{
    uint32_t it, v, i, total = 0, nd = 0, k;
    size_t at = a;
    unsigned mb = (nlanes + 7) / 8;
    r[0] = r[1] = r[2] = r[3] = a;
    r[4] = e;
    if (!varint_get(sec, e, &at, &it)) return;
    for (i = 0; i < it && i < 256; i++) {
        if (!varint_get(sec, e, &at, &v)) return;
        total += v;
    }
    for (i = 0; i < total; i++)
        if (!(at = skip_varint(sec, e, at))) return;
    if (!varint_get(sec, e, &at, &nd)) return;
    r[1] = at;
    r[2] = at + (size_t)nd * mb;
    if (r[2] > e) r[2] = e;
    at = r[2];
    for (k = 0; k < total && at < e; k++) at = nd <= 256 ? at + 1 : (skip_varint(sec, e, at) ? skip_varint(sec, e, at) : e);
    r[3] = at < e ? at : e;
}

/* RCS_PROJ's regions from the count c0 the block starts with: [0] the control values, [1] the
   codes, [2] the survivors' columns, [3] the births', [4] the tail's, [5] the end */
static void pool_regions(const unsigned char* sec, size_t a, size_t e, unsigned c0, size_t r[6])
{
    uint32_t it, v, i, k;
    uint64_t ns = 0, nb = 0, nt = 0, n, c = c0;
    size_t at = a;
    r[0] = r[1] = r[2] = r[3] = r[4] = a;
    r[5] = e;
    if (!varint_get(sec, e, &at, &it)) return;
    for (i = 0; i < it && i < 256; i++) {
        uint64_t c1, nd, t;
        if (!varint_get(sec, e, &at, &v)) return;
        c1 = (uint64_t)((int64_t)c + unzz64(v));
        if (!(at = skip_varint(sec, e, at)) || !varint_get(sec, e, &at, &v)) return;
        nd = v;
        for (k = 0; k < nd; k++)
            if (!(at = skip_varint(sec, e, at))) return;
        if (!varint_get(sec, e, &at, &v)) return;
        t = v;
        for (k = 0; k < t; k++)
            if (!(at = skip_varint(sec, e, at))) return;
        ns += c - nd;
        nb += c1 - (c - nd);
        nt += t;
        c = c1;
    }
    r[1] = at;
    for (n = 0; n < nb + nt; n++)
        if (!(at = skip_varint(sec, e, at))) return;
    r[2] = at;
    for (n = 0; n < PDW * ns; n++)
        if (!(at = skip_varint(sec, e, at))) return;
    r[3] = at;
    for (n = 0; n < PDW * nb; n++)
        if (!(at = skip_varint(sec, e, at))) return;
    r[4] = at;
}

/* A record frame's section [a, e): the end of its column table in *tab and its columns' bounds,
   col[0] the first's start and col[k + 1] column k's end. The columns read, else 0 (and *tab = a). */
static unsigned frame_regions(const unsigned char* sec, size_t a, size_t e, size_t* tab,
                              size_t col[RECF_COLS_MAX + 1])
{
    size_t at = a + 1, len[RECF_COLS_MAX];
    uint32_t v, ncols, i;
    *tab = a;
    if (a >= e || !varint_get(sec, e, &at, &ncols) || ncols < 1 || ncols > RECF_COLS_MAX) return 0;
    for (i = 0; i < ncols; i++) {
        if (!varint_get(sec, e, &at, &v)) return 0;
        len[i] = v;
    }
    *tab = at;
    col[0] = at;
    for (i = 0; i < ncols; i++) {
        if (len[i] > e - col[i]) return 0;
        col[i + 1] = col[i] + len[i];
    }
    return ncols;
}

static const char* const SECTION_NAME[RCS_COUNT] = { "kinds", "units", "proj", "cells", "orders",
                                                     "events", "other", "state" };

typedef struct Case {
    unsigned type;                  /* 0 the block mutated, 1 the header's entries mutated */
    unsigned flags;
    RecBytes body;
    unsigned char ent[3][8 + 24 * 8 + 4 * RECC_LANES_MAX];
    unsigned nent[3], has[3];
} Case;

static void mutate_byte(unsigned char* p)
{
    switch (rnd() % 4) {
    case 0: *p = (unsigned char)rnd(); break;
    case 1: *p += 1; break;
    case 2: *p -= 1; break;
    default: *p ^= (unsigned char)(1u << (rnd() % 8)); break;
    }
}

/* The mutations each section takes (mutate's `what`), for --section: a byte inserted or removed
   in the section (10) and the section's own */
static unsigned what_of(int only)
{
    static const unsigned char unit[] = { 4, 5, 6, 7, 10 }, proj[] = { 11, 12, 13, 14, 15, 10 },
                               frame[] = { 16, 17, 10 };
    switch (only) {
    case RCS_KINDS: return rnd() & 1 ? 3 : 10;
    case RCS_UNITS: return unit[rndn(sizeof unit)];
    case RCS_PROJ:  return proj[rndn(sizeof proj)];
    case RCS_OTHER: return rnd() & 1 ? 8 : 10;
    default:        return frame[rndn(sizeof frame)];
    }
}

/* one mutation of the block; `only` >= 0 keeps it to that section */
static void mutate(Case* c, const unsigned char* sec, size_t n, unsigned flags, const Header* h, unsigned nlanes,
                   unsigned pc0, int only)
{
    static const unsigned char FRAME_SEC[4] = { RCS_CELLS, RCS_ORDERS, RCS_EVENTS, RCS_STATE };
    size_t off[RCS_COUNT + 1], i, at, r[6];
    unsigned what = only >= 0 ? what_of(only) : (unsigned)(rnd() % 19);
    c->type = 0;
    c->flags = flags;
    c->body.n = 0;
    rb_put(&c->body, sec, n);
    off[0] = sizeof(RecSecHead);
    for (i = 0; i < RCS_COUNT; i++) off[i + 1] = off[i] + get32(sec + 8 + 4 * i);
    switch (what) {
    case 0:
        c->flags ^= 1u << (rnd() % 8);
        break;
    case 1:                                       /* a head field */
        mutate_byte(c->body.p + rndn(sizeof(RecSecHead)));
        break;
    case 2: {                                     /* one section's bytes moved to another */
        size_t a = rndn(RCS_COUNT), b = rndn(RCS_COUNT);
        uint32_t la = get32(c->body.p + 8 + 4 * a), lb = get32(c->body.p + 8 + 4 * b);
        unsigned d = (unsigned)(1 + rndn(3));
        if (lb >= d) {
            put32(c->body.p + 8 + 4 * a, la + d);
            put32(c->body.p + 8 + 4 * b, lb - d);
        }
        break;
    }
    case 3:                                       /* the kinds' runs */
        if (off[1] > off[0]) mutate_byte(c->body.p + off[0] + rndn(off[1] - off[0]));
        break;
    case 4: case 5: case 6: case 7: {             /* the unit frame: prefix, masks, ids, columns */
        unit_regions(sec, off[RCS_UNITS], off[RCS_UNITS + 1], nlanes, r);
        i = what - 4;
        if (r[i + 1] > r[i]) mutate_byte(c->body.p + r[i] + rndn(r[i + 1] - r[i]));
        else if (off[2] > off[1]) mutate_byte(c->body.p + off[1] + rndn(off[2] - off[1]));
        break;
    }
    case 11: case 12: case 13: case 14: case 15: {   /* the pool frame: control, codes, columns */
        pool_regions(sec, off[RCS_PROJ], off[RCS_PROJ + 1], pc0, r);
        i = what - 11;
        if (r[i + 1] > r[i]) mutate_byte(c->body.p + r[i] + rndn(r[i + 1] - r[i]));
        else if (off[RCS_PROJ + 1] > off[RCS_PROJ]) mutate_byte(c->body.p + off[RCS_PROJ] + rndn(off[RCS_PROJ + 1] - off[RCS_PROJ]));
        break;
    }
    case 8:                                       /* RCS_OTHER's bodies */
        if (off[RCS_OTHER + 1] > off[RCS_OTHER])
            mutate_byte(c->body.p + off[RCS_OTHER] + rndn(off[RCS_OTHER + 1] - off[RCS_OTHER]));
        break;
    case 9:                                       /* a truncation */
        c->body.n = rndn(n);
        break;
    case 10: {                                    /* a byte inserted or removed in a section */
        size_t s = only >= 0 ? (size_t)only : rndn(RCS_COUNT), len = off[s + 1] - off[s];
        at = off[s] + rndn(len + 1);
        if (rnd() & 1 || !len) {
            rb_put(&c->body, "", 1);
            memmove(c->body.p + at + 1, c->body.p + at, c->body.n - 1 - at);
            c->body.p[at] = (unsigned char)rnd();
            put32(c->body.p + 8 + 4 * s, (uint32_t)len + 1);
        } else {
            if (at == off[s + 1]) at--;
            memmove(c->body.p + at, c->body.p + at + 1, c->body.n - at - 1);
            c->body.n--;
            put32(c->body.p + 8 + 4 * s, (uint32_t)len - 1);
        }
        break;
    }
    case 16: case 17: {                           /* a record frame: its column table, a column */
        size_t s = only >= 0 ? (size_t)only : FRAME_SEC[rndn(4)], tab, col[RECF_COLS_MAX + 1];
        unsigned nc = frame_regions(sec, off[s], off[s + 1], &tab, col), k;
        if (what == 17 && nc) {
            k = (unsigned)rndn(nc);
            if (col[k + 1] > col[k]) {
                mutate_byte(c->body.p + col[k] + rndn(col[k + 1] - col[k]));
                break;
            }
        }
        if (tab > off[s]) mutate_byte(c->body.p + off[s] + rndn(tab - off[s]));
        else if (off[s + 1] > off[s]) mutate_byte(c->body.p + off[s] + rndn(off[s + 1] - off[s]));
        break;
    }
    default: {                                    /* the header's codec entries */
        unsigned e = (unsigned)rndn(3);
        if (!(flags & RBF_FRAME_RESET)) {         /* only a restart decodes from new params */
            mutate_byte(c->body.p + sizeof(RecSecHead) + rndn(n - sizeof(RecSecHead)));
            break;
        }
        c->type = 1;
        for (i = 0; i < 3; i++) {
            c->has[i] = h->e[i] != NULL;
            c->nent[i] = h->ne[i] <= sizeof c->ent[i] ? h->ne[i] : 0;
            if (h->e[i]) memcpy(c->ent[i], h->e[i], c->nent[i]);
        }
        switch (rnd() % 5) {
        case 0:
            if (c->nent[e]) mutate_byte(c->ent[e] + rndn(c->nent[e]));
            break;
        case 4:                                   /* a constant: gravity, sea level, a def value */
            if (c->nent[1] == 8) mutate_byte(c->ent[1] + rndn(5));
            if (c->nent[2] >= 32) {
                size_t row = rndn((c->nent[2] - 8) / 24);
                mutate_byte(c->ent[2] + 8 + 24 * row + 4 + rndn(20));
                if (rnd() & 1) c->ent[2][8 + 24 * row + 2] = (unsigned char)rnd();
            }
            break;
        case 1:
            if (c->nent[e]) c->nent[e] = (unsigned)rndn(c->nent[e]);
            break;
        case 2:
            c->has[e] = !c->has[e];
            break;
        default:
            if (c->nent[e] > 4) {
                size_t a = 4 + rndn(c->nent[e] - 4), b = 4 + rndn(c->nent[e] - 4);
                unsigned char t = c->ent[e][a];
                c->ent[e][a] = c->ent[e][b];
                c->ent[e][b] = t;
            }
            break;
        }
        break;
    }
    }
}

/* the reader's handling of a block as the differential defines it: 1 with `out`, or 0 refused */
static int case_run(Case* c, RecCodec* d, const RecCodec* from, const RecCodecParams* p, const World* w,
                    const unsigned char* sec, size_t n, RecBytes* out)
{
    char why[256];
    RecFrameWorld v = world_view(w);
    codec_copy(d, from);
    if (c->type == 1) {
        RecCodecParams q;
        if (!reccodec_params(&q, p->nslots, p->nproj, p->gw, p->gh, p->nwrecks, c->has[0] ? c->ent[0] : NULL, c->nent[0],
                             c->has[1] ? c->ent[1] : NULL, c->nent[1], c->has[2] ? c->ent[2] : NULL,
                             c->nent[2], why, sizeof why) || !q.usable)
            return 0;
        reccodec_restart(d, &q, w->unit, w->live, w->pool, w->pcount, w->phw, &v);
        return reccodec_decode(d, sec, n, out, why, sizeof why);
    }
    if (c->flags & ~(unsigned)(RBF_STORED | RBF_SECTIONS | RBF_FRAME_RESET | RBF_REFRESH)) return 0;
    if ((c->flags & RBF_FRAME_RESET) && !(c->flags & RBF_SECTIONS)) return 0;
    if (!(c->flags & RBF_SECTIONS)) {
        out->n = 0;
        rb_put(out, c->body.p, c->body.n);
        return 1;
    }
    if (c->flags & RBF_FRAME_RESET) reccodec_restart(d, p, w->unit, w->live, w->pool, w->pcount, w->phw, &v);
    return reccodec_decode(d, c->body.p, c->body.n, out, why, sizeof why);
}

static int cmd_fuzz(const char* in, uint64_t seed, unsigned ncases, const char* casesp, int only)
{
    Reading g;
    RecCodec* d = NULL;
    RecBytes out;
    Case c;
    FILE* cf = NULL;
    unsigned nsect = 0, done = 0, accepted = 0, k2;
    int k, control = -1;
    memset(&out, 0, sizeof out);
    memset(&c, 0, sizeof c);
    rng_state = seed * UINT64_C(0x9E3779B97F4A7C15) + 1;
    /* count the sectioned blocks first, to spread the cases over them */
    if (!reading_open(&g, in)) return 2;
    while ((k = next_block(&g.r)) > 0) nsect += g.r.b.kind == RB_STREAM && (g.r.b.flags & RBF_SECTIONS);
    if (k < 0 || !nsect) {
        fprintf(stderr, "codectest: %s: %s\n", in, k < 0 ? g.r.why : "no sectioned block");
        return 2;
    }
    reading_close(&g);
    if (!reading_open(&g, in)) return 2;
    if (casesp && !(cf = fopen(casesp, "wb"))) return 2;
    k2 = 0;
    while ((k = next_block(&g.r)) > 0) {
        RecBlock* b = &g.r.b;
        double t;
        /* the cases of this block, from the state the block starts from */
        if (b->kind == RB_STREAM && (b->flags & RBF_SECTIONS) && g.dec) {
            unsigned want = (unsigned)(((uint64_t)ncases * (k2 + 1)) / nsect - ((uint64_t)ncases * k2) / nsect), i;
            if (!d && !(d = reccodec_new(&g.p))) {
                fprintf(stderr, "codectest: no memory for the fuzz's decoder\n");
                return 2;
            }
            for (i = 0; i < want; i++) {
                int ok;
                uint64_t hsh = 0;
                mutate(&c, g.r.bytes, b->raw, b->flags, &g.h, g.p.nlanes,
                       (b->flags & RBF_FRAME_RESET) ? g.w.pcount : g.dec->pcount, only);
                ok = case_run(&c, d, g.dec, &g.p, &g.w, g.r.bytes, b->raw, &out);
                accepted += ok;
                if (ok) hsh = fnv64(out.p, out.n);
                if (cf) {
                    uint32_t head[8];
                    unsigned e;
                    head[0] = g.r.ordinal;
                    head[1] = c.type;
                    head[2] = c.flags;
                    head[3] = (uint32_t)c.body.n;
                    head[4] = (uint32_t)ok;
                    head[5] = ok ? (uint32_t)out.n : 0;
                    head[6] = (uint32_t)hsh;
                    head[7] = (uint32_t)(hsh >> 32);
                    fwrite(head, 1, sizeof head, cf);
                    if (c.type == 0) {
                        fwrite(c.body.p, 1, c.body.n, cf);
                    } else {
                        for (e = 0; e < 3; e++) {
                            uint32_t eh[2] = { c.has[e], c.nent[e] };
                            fwrite(eh, 1, 8, cf);
                            fwrite(c.ent[e], 1, c.nent[e], cf);
                        }
                    }
                }
                done++;
            }
            /* The control: the block itself, unmutated, decoded from the copy after the cases
               have left their own state in d. It must give the records the reading's decoder gives
               below; otherwise codec_copy misses state the decoder keeps between blocks, and every
               case above started from a state no reader holds. */
            if (want) {
                c.type = 0;
                c.flags = b->flags;
                c.body.n = 0;
                rb_put(&c.body, g.r.bytes, b->raw);
                control = case_run(&c, d, g.dec, &g.p, &g.w, g.r.bytes, b->raw, &out);
            }
            k2++;
        }
        /* then the block itself, as a reading takes it */
        g.r.at -= 32 + b->packed;
        g.r.ordinal--;
        if (reading_next(&g, &t) < 0) {
            fprintf(stderr, "codectest: %s: %s\n", in, g.r.why);
            k = -1;
            break;
        }
        if (control >= 0 && (!control || out.n != g.pay.n || memcmp(out.p, g.pay.p, out.n))) {
            fprintf(stderr, "codectest: %s: block %u from the fuzz's copy of the decoder %s: codec_copy "
                    "misses state the decoder keeps between blocks\n", in, g.r.ordinal,
                    control ? "gives other records than the reading's" : "is refused, the reading's is not");
            k = -2;
            break;
        }
        control = -1;
    }
    if (cf) fclose(cf);
    printf("fuzz: %u cases over %u sectioned blocks%s%s, %u accepted, %u refused\n", done, nsect,
           only >= 0 ? ", section " : "", only >= 0 ? SECTION_NAME[only] : "", accepted, done - accepted);
    reccodec_free(d);
    rb_free(&out);
    rb_free(&c.body);
    reading_close(&g);
    return k == -2 ? 1 : k < 0 ? 2 : 0;
}

int main(int argc, char** argv)
{
    if (argc >= 4 && !strcmp(argv[1], "transcode")) {
        int every = -1, skirmish = 0, i;
        for (i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--skirmish")) {
                skirmish = 1;
            } else if (!strcmp(argv[i], "--snap-every") && i + 1 < argc) {
                /* digits alone: a lenient parse would read a typo as 0, which keeps no snapshot */
                const char* p = argv[++i];
                unsigned n = 0;
                do {
                    if (*p < '0' || *p > '9' || (n = n * 10 + (unsigned)(*p - '0')) > 1000000) {
                        fprintf(stderr, "codectest: --snap-every takes a count from 0 to 1000000, "
                                "in digits alone, not %s\n", argv[i]);
                        return 2;
                    }
                } while (*++p);
                every = (int)n;
            } else {
                fprintf(stderr, "codectest: transcode does not take %s\n", argv[i]);
                return 2;
            }
        }
        return cmd_transcode(argv[2], argv[3], every, skirmish);
    }
    if ((argc == 4 || (argc == 5 && !strcmp(argv[4], "--exact"))) && !strcmp(argv[1], "lossy"))
        return cmd_lossy(argv[2], argv[3], argc == 5);
    if (argc == 4 && !strcmp(argv[1], "decode")) return cmd_decode(argv[2], argv[3]);
    if (argc == 2 && !strcmp(argv[1], "outcomes")) return cmd_outcomes();
#ifdef _WIN32
    if (argc == 3 && !strcmp(argv[1], "sprays")) return cmd_sprays(argv[2]);
#endif
    if (argc >= 5 && !strcmp(argv[1], "fuzz")) {
        /* fuzz IN SEED N [CASES] [--section NAME], the option anywhere after N */
        const char* cases = NULL;
        int only = -1, i, k;
        for (i = 5; i < argc; i++) {
            if (!strcmp(argv[i], "--section") && i + 1 < argc) {
                for (k = 0; k < RCS_COUNT && strcmp(argv[i + 1], SECTION_NAME[k]); k++) ;
                if (k == RCS_COUNT) {
                    fprintf(stderr, "codectest: no section %s\n", argv[i + 1]);
                    return 2;
                }
                only = k;
                i++;
            } else if (!cases) {
                cases = argv[i];
            } else {
                fprintf(stderr, "codectest: fuzz takes one CASES file\n");
                return 2;
            }
        }
        return cmd_fuzz(argv[2], strtoull(argv[3], NULL, 10), (unsigned)strtoul(argv[4], NULL, 10),
                        cases, only);
    }
    if (argc == 2 && !strcmp(argv[1], "sin")) {
        int i;
        for (i = 0; i < 512; i++) printf("%d\n", SIN[i]);
        return 0;
    }
    fprintf(stderr, "usage: codectest transcode IN OUT [--snap-every N] [--skirmish] | lossy IN OUT [--exact] | decode IN OUT | outcomes | sprays IN | fuzz IN SEED N [CASES] [--section NAME] | sin\n");
    return 2;
}
