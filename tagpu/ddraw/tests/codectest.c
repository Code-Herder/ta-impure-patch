/* codectest.c -- the stream codec's self-test (src/tagpu_reccodec.c), driven by codectest.py.

     codectest transcode IN OUT     every stream block of a recording decoded to its records and
                                    written again through reccodec_stream_block, restarting where
                                    the writer restarts; a line per block (raw, section and packed
                                    bytes, milliseconds) and the outcomes
     codectest decode IN OUT        every stream block's records, C, framed for codectest.py; a
                                    line per block with its milliseconds
     codectest outcomes             synthetic payloads that must be SECTIONS, DECLINED (a delta
                                    naming an unchanged dword; a correction on a delta's slot) and
                                    MISMATCH (the check decoder restarted from another world), each
                                    followed by a restarting SECTIONS block
     codectest fuzz IN SEED N [CASES]
                                    N mutations of the recording's sectioned blocks (flags, the
                                    head, section lengths, kinds, gaps, masks, ids, columns,
                                    bodies, the header's codec entries, truncations), each decoded
                                    from a copy of the state the block starts from; with CASES,
                                    every case and its result for codectest.py's differential
     codectest sin                  the sine table, for codectest.py's check against its formula

   IN is a recording of format 8. Exit 0 when every check holds, 1 when one fails, 2 when a file
   cannot be read. The codec is included rather than linked: the fuzz copies a decoder's state,
   which the codec's interface keeps opaque. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/tagpu_reccodec.c"

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

/* ---- a recording of format 8, block by block ------------------------------------------------- */

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
    allowed = RBF_STORED | (r->b.kind == RB_STREAM ? RBF_SECTIONS | RBF_FRAME_RESET : 0);
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
    unsigned format, slots;
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
        if (tag == RT_SLOTS && len == 4) h->slots = get32(b);
        k = tag == RT_CODEC ? 0 : tag == RT_PHYSICS ? 1 : tag == RT_AIR ? 2 : 3;
        if (k < 3) {
            if (h->e[k]) REFUSE("header tag %u twice", tag);
            h->e[k] = b;
            h->ne[k] = len;
        }
        at += (4 + (size_t)len + 3) & ~(size_t)3;
    }
    if (h->format != 8) REFUSE("format %u, not 8", h->format);
    return 1;
}

/* the reader's copy of the units */
typedef struct World { unsigned nslots; uint32_t* unit; unsigned char* live; } World;

static void world_reset(World* w, unsigned nslots)
{
    free(w->unit);
    free(w->live);
    w->nslots = nslots;
    w->unit = (uint32_t*)calloc(nslots, IMG);
    w->live = (unsigned char*)calloc(nslots, 1);
}

static void world_free(World* w)
{
    free(w->unit);
    free(w->live);
    memset(w, 0, sizeof *w);
}

static void world_apply(World* w, const unsigned char* pl, size_t n)
{
    size_t at = 0;
    while (n - at >= 4) {
        unsigned kind = get16(pl + at), len = get16(pl + at + 2), s, i, k, nb, ok;
        size_t rl = ((size_t)4 + len + 3) & ~(size_t)3;
        const unsigned char* b = pl + at + 4;
        if (rl > n - at) break;
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

/* the live slots of the codec's copy against the world's */
static int world_same(const World* w, const RecCodec* c)
{
    unsigned s;
    for (s = 1; s < w->nslots; s++) {
        if (!w->live[s] != !c->live[s]) return 0;
        if (w->live[s] && memcmp(w->unit + (size_t)s * NDW, c->unit + (size_t)s * NDW, IMG)) return 0;
    }
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
        if (!reccodec_params(&g->p, g->h.slots, g->h.e[0], g->h.ne[0], g->h.e[1], g->h.ne[1], g->h.e[2],
                             g->h.ne[2], g->r.why, sizeof g->r.why))
            return -1;
        if (g->dec && g->w.nslots != g->h.slots) {
            snprintf(g->r.why, sizeof g->r.why, "a segment of %u slots after %u", g->h.slots, g->w.nslots);
            return -1;
        }
        world_reset(&g->w, g->h.slots);
        if (!g->dec) g->dec = reccodec_new(g->h.slots);
        if (!g->dec || !g->w.unit || !g->w.live) {
            snprintf(g->r.why, sizeof g->r.why, "no memory");
            return -1;
        }
        reccodec_invalidate(g->dec);
        g->in_segment = 1;
        return 1;
    }
    if (g->r.b.kind != RB_STREAM) return 1;
    if (g->r.b.flags & RBF_SECTIONS) {
        clock_t t0 = clock();
        if (!g->p.usable) {
            snprintf(g->r.why, sizeof g->r.why, "a sectioned block in a segment without RT_CODEC and RT_PHYSICS");
            return -1;
        }
        if (g->r.b.flags & RBF_FRAME_RESET) reccodec_restart(g->dec, &g->p, g->w.unit, g->w.live);
        if (!reccodec_decode(g->dec, g->r.bytes, g->r.b.raw, &g->pay, g->r.why, sizeof g->r.why)) return -1;
        *ms = ms_since(t0);
    } else {
        reccodec_invalidate(g->dec);
        g->pay.n = 0;
        g->pay.oom = 0;
        rb_put(&g->pay, g->r.bytes, g->r.b.raw);
    }
    world_apply(&g->w, g->pay.p, g->pay.n);
    return 1;
}

/* ---- transcode, decode ------------------------------------------------------------------------ */

static int cmd_transcode(const char* in, const char* outp)
{
    Reading g;
    RecStream st;
    FILE* out;
    int k, on = 0, bad = 0;
    double ms, tot = 0, worst = 0;
    size_t raw = 0, written = 0, secbytes = 0, unitbytes = 0;
    unsigned nblk = 0;
    memset(&st, 0, sizeof st);
    if (!reading_open(&g, in)) return 2;
    out = fopen(outp, "wb");
    if (!out) return 2;
    fwrite(REC_MAGIC, 1, 8, out);
    printf("block kind first last raw section units packed flags outcome ms\n");
    while ((k = reading_next(&g, &ms)) > 0) {
        RecBlock* b = &g.r.b;
        if (b->kind == RB_HEADER) {
            if (on) reccodec_stream_end(&st);
            on = g.p.usable && reccodec_stream_begin(&st, &g.p);
            put_block(out, b->kind, RBF_STORED, b->first, b->last, b->raw, g.r.bytes, b->raw);
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
            }
            put_block(out, b->kind, flags, b->first, b->last,
                      (uint32_t)(o == RECC_SECTIONS ? st.sec.n : g.pay.n), body, bn);
            printf("%u stream %lu %lu %lu %lu %lu %lu %#x %s %.2f\n", g.r.ordinal, (unsigned long)b->first,
                   (unsigned long)b->last, (unsigned long)g.pay.n,
                   (unsigned long)(o == RECC_SECTIONS ? st.sec.n : 0),
                   (unsigned long)(o == RECC_SECTIONS ? sh->len[RCS_UNITS] : 0), (unsigned long)bn, flags,
                   o == RECC_SECTIONS ? "sections" : o == RECC_DECLINED ? "DECLINED" : "MISMATCH", t);
            if (o != RECC_SECTIONS) bad = 1;
            if (!world_same(&g.w, st.enc)) {
                printf("codectest: the encoder's copy is not the world's after block %u\n", g.r.ordinal);
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
            put_block(out, b->kind, RBF_STORED, b->first, b->last, b->raw, g.r.bytes, b->raw);
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
           "containers %lu (unit frames %lu), written %lu; encode+check %.0f ms, at most %.2f ms a block\n",
           nblk, st.sectioned, st.declined, st.mismatched, (unsigned long)raw, (unsigned long)secbytes,
           (unsigned long)unitbytes, (unsigned long)written, tot, worst);
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

/* Six iterations of five units: two ground movers and an aircraft moving, a still unit, and one
   whose plain dword changes unpredictably. `force`: in the fourth iteration a delta names a dword
   of the still unit it does not change; `correct`: a correction of a unit whose delta the same
   iteration carries. */
static void synth_block(RecBytes* b, uint32_t units[][NDW], uint32_t* gt, int create, int force, int correct)
{
    static uint32_t prev[6][NDW];
    unsigned it, s;
    b->n = 0;
    for (it = 0; it < 6; it++, (*gt)++) {
        uint32_t c = 0;
        rec_put(b, RK_TICK, gt, 4);
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
        if (flags & RBF_FRAME_RESET) reccodec_restart(r, p, w->unit, w->live);
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

static int cmd_outcomes(void)
{
    static uint32_t units[6][NDW];
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
    if (!reccodec_params(&p, 64, codec, sizeof codec, phys, 8, air, sizeof air, NULL, 0) ||
        !reccodec_stream_begin(&st, &p) || !(rd = reccodec_new(64)))
        return 2;
    world_reset(&w, 64);
    for (block = 0; block < 8; block++) {
        static const int want[8] = { RECC_SECTIONS, RECC_SECTIONS, RECC_DECLINED, RECC_SECTIONS,
                                     RECC_DECLINED, RECC_SECTIONS, RECC_MISMATCH, RECC_SECTIONS };
        static const char* what[8] = {
            "SECTIONS, the segment's first block", "SECTIONS, continuing",
            "DECLINED: a delta naming an unchanged dword", "SECTIONS, restarting after it",
            "DECLINED: a correction on a delta's slot", "SECTIONS, restarting after it",
            "MISMATCH: the check decoder restarted from another world", "SECTIONS, restarting after it" };
        char line[128];
        int o;
        synth_block(&y, units, &gt, block == 0, block == 2, block == 4);
        if (block == 6) {
            static uint32_t other[64][NDW];
            static unsigned char live[64];
            memcpy(other, reccodec_units(st.enc), sizeof other);
            memcpy(live, reccodec_live(st.enc), sizeof live);
            other[2][20] ^= 0x10;
            reccodec_restart(st.enc, &st.p, reccodec_units(st.enc), reccodec_live(st.enc));
            reccodec_restart(st.chk, &st.p, &other[0][0], live);
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
    }
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

/* a copy of a decoder's state: what the frame reads and writes */
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

static void mutate(Case* c, const unsigned char* sec, size_t n, unsigned flags, const Header* h, unsigned nlanes)
{
    size_t off[RCS_COUNT + 1], i, at, r[5];
    unsigned what = (unsigned)(rnd() % 12);
    c->type = 0;
    c->flags = flags;
    c->body.n = 0;
    rb_put(&c->body, sec, n);
    off[0] = 36;
    for (i = 0; i < RCS_COUNT; i++) off[i + 1] = off[i] + get32(sec + 8 + 4 * i);
    switch (what) {
    case 0:
        c->flags ^= 1u << (rnd() % 8);
        break;
    case 1:                                       /* a head field */
        mutate_byte(c->body.p + rndn(36));
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
    case 8: {                                     /* a body section */
        size_t s = RCS_PROJ + rndn(RCS_COUNT - RCS_PROJ);
        if (off[s + 1] > off[s]) mutate_byte(c->body.p + off[s] + rndn(off[s + 1] - off[s]));
        break;
    }
    case 9:                                       /* a truncation */
        c->body.n = rndn(n);
        break;
    case 10: {                                    /* a byte inserted or removed in a section */
        size_t s = rndn(RCS_COUNT), len = off[s + 1] - off[s];
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
    default: {                                    /* the header's codec entries */
        unsigned e = (unsigned)rndn(3);
        if (!(flags & RBF_FRAME_RESET)) {         /* only a restart decodes from new params */
            mutate_byte(c->body.p + 36 + rndn(n - 36));
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
    codec_copy(d, from);
    if (c->type == 1) {
        RecCodecParams q;
        if (!reccodec_params(&q, p->nslots, c->has[0] ? c->ent[0] : NULL, c->nent[0],
                             c->has[1] ? c->ent[1] : NULL, c->nent[1], c->has[2] ? c->ent[2] : NULL,
                             c->nent[2], why, sizeof why) || !q.usable)
            return 0;
        reccodec_restart(d, &q, w->unit, w->live);
        return reccodec_decode(d, sec, n, out, why, sizeof why);
    }
    if (c->flags & ~(unsigned)(RBF_STORED | RBF_SECTIONS | RBF_FRAME_RESET)) return 0;
    if ((c->flags & RBF_FRAME_RESET) && !(c->flags & RBF_SECTIONS)) return 0;
    if (!(c->flags & RBF_SECTIONS)) {
        out->n = 0;
        rb_put(out, c->body.p, c->body.n);
        return 1;
    }
    if (c->flags & RBF_FRAME_RESET) reccodec_restart(d, p, w->unit, w->live);
    return reccodec_decode(d, c->body.p, c->body.n, out, why, sizeof why);
}

static int cmd_fuzz(const char* in, uint64_t seed, unsigned ncases, const char* casesp)
{
    Reading g;
    RecCodec* d = NULL;
    RecBytes out;
    Case c;
    FILE* cf = NULL;
    unsigned nsect = 0, done = 0, accepted = 0, k2;
    int k;
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
            if (!d) d = reccodec_new(g.dec->nslots);
            for (i = 0; i < want; i++) {
                int ok;
                uint64_t hsh = 0;
                mutate(&c, g.r.bytes, b->raw, b->flags, &g.h, g.p.nlanes);
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
    }
    if (cf) fclose(cf);
    printf("fuzz: %u cases over %u sectioned blocks, %u accepted, %u refused\n", done, nsect, accepted,
           done - accepted);
    reccodec_free(d);
    rb_free(&out);
    rb_free(&c.body);
    reading_close(&g);
    return k < 0 ? 2 : 0;
}

int main(int argc, char** argv)
{
    if (argc == 4 && !strcmp(argv[1], "transcode")) return cmd_transcode(argv[2], argv[3]);
    if (argc == 4 && !strcmp(argv[1], "decode")) return cmd_decode(argv[2], argv[3]);
    if (argc == 2 && !strcmp(argv[1], "outcomes")) return cmd_outcomes();
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "fuzz"))
        return cmd_fuzz(argv[2], strtoull(argv[3], NULL, 10), (unsigned)strtoul(argv[4], NULL, 10),
                        argc == 6 ? argv[5] : NULL);
    if (argc == 2 && !strcmp(argv[1], "sin")) {
        int i;
        for (i = 0; i < 512; i++) printf("%d\n", SIN[i]);
        return 0;
    }
    fprintf(stderr, "usage: codectest transcode IN OUT | decode IN OUT | outcomes | fuzz IN SEED N [CASES] | sin\n");
    return 2;
}
