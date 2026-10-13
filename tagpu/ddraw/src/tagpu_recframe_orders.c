/* tagpu_recframe_orders.c -- the orders frame (tagpu_recfmt.h, RCS_ORDERS; tagpu_recframe.h says
   what a frame is and what it shares): every RK_ORDERS of a sectioned block.

   ENGINE FACTS. A node's timer 0x439E80 (thiscall (node, ticks), `ret 4`, 75 call sites) sets
   +0x06 |= 1 and +0x0A = [main+0x38A47] + ticks (DISASSEMBLED 0x439E80..0x439E9A), so a list the
   order handlers re-arm changes every tick and is sent again whole, at the length it had.
   MEASURED 2026-10-10 on format-9 recordings: 286,289 of the battle's 293,393 sends meet a kept
   list of the same length and every one changed its head node (node 2: 59 times, node 1: once);
   Town & Country 161,642 of 167,290, the air war 48,619 of 52,199; the longest list held 3 nodes.
   So a list is coded against the last image of the same (slot, list), node by node, as the
   dwords that changed.

   In a skirmish stream the lossy filter holds a main list's handler state between refreshes
   (replay-hold.tsv), so a list keeps its nodes but loses one -- its head done -- or gains one
   in front of the others, and node i no longer meets kept node i. A node's action byte +0x04 and
   its issue time +0x46 stay with it -- the order constructor 0x43A0C0 stores the action at
   0x43A0E0 and GameTime [main+0x38A47] at 0x43A12E, and no other writer of +0x46 was traced
   (DISASSEMBLED, exe-reverse-engineering.md, the record frames) -- so there each node is matched
   to the kept node that has both. MEASURED on the size
   research's lossy hour: −119,291 bytes; on the exact hour the match costs +295,783, so a stream
   without RT_HOLD keeps scheme 1.

   THE BYTES. Scheme 1 in a segment whose header has no RT_HOLD (a multiplayer one), scheme 2 in
   one that has it (RecCodecParams' `hold`); a reader takes only its segment's. Scheme 1 is 26
   columns, for each record in record order:
       slot     varint zz32(slot - the previous record's slot in this iteration, 0 at its start)
       key      u8: bit 0 the list (0 the main list, 1 the sub list), bit 1 WHOLE, bit 2 CHUNK
                (only with WHOLE); with WHOLE a varint total follows, with CHUNK a varint first
                and a varint k
       mask     with neither bit, per node of the kept list (the record's total is the kept
                total, first 0, k the total): a varint of 22 bits, bit d set when dword d
                differs from the kept node's
       d0..d21  the dwords each mask names, u32 raw, one column per dword
       whole    with WHOLE, the k node images raw (REC_ORDER_IMAGE bytes each)
   WHOLE alone is a list in one record (first 0, k the total); WHOLE | CHUNK a record of a list
   longer than REC_ORDERS_CHUNK, or of any other range. The encoder takes the masks whenever the
   (slot, list) holds a kept list of the record's total and the record is the whole list.
   Scheme 2 is 27 columns, scheme 1's and
       idx      per node of the record, in order: a varint, 0 for a node whose image follows raw
                in `whole`, else 1 + j for the kept list's node j, which a mask and its dwords
                then follow as scheme 1's do against node j
   the key as scheme 1's, but every record's nodes coded by idx: without WHOLE the record is the
   whole list at the kept list's length. The encoder matches each node to the first kept node
   after the last one matched, within ORDERS_SCAN of it, with the same +0x04 byte and +0x46 dword;
   that is a policy, the decoder reads the choice.

   THE STATE, cleared at every reset. Per (slot, list) its place in the arena (offset and capacity,
   in nodes) and its kept list: the node images of the last record of that (slot, list) when that
   record was the whole list, or none. A record that is not the whole list drops the kept one, and
   in scheme 2 a unit's RK_UNIT_GONE drops both of its slot's. A whole list fits in its place when
   its total is at most the place's capacity, else it takes a new place of exactly its total at the
   arena's end, if the arena then holds at most ORDERS_HELD nodes; else it is not kept and the old
   place stays for a later, shorter list. The places are a function of the records since the reset
   alone, so the encoder and every decoder keep the same lists. The arena's memory doubles up to
   ORDERS_HELD nodes as places are taken; a failed allocation latches `oom` -- the encoder declines
   the block, the decoder refuses it -- and never changes what is kept. A record the frame cannot
   carry latches `unfit` in the encoder: the frame never declines mid-block, its enc_write does.
   MEMORY: 12 bytes a (slot, list), and the arena up to ORDERS_HELD x REC_ORDER_IMAGE bytes (5.8
   MB).

   THE BOUNDS. 1 <= slot < nslots; a key of bits 0..2 with CHUNK only beside WHOLE; total <=
   REC_ORDERS_MAX; k <= REC_ORDERS_CHUNK, first + k <= total, and k >= 1 unless the total is 0 (an
   emptied list is one record with no nodes, tagpu_recfmt.h RK_ORDERS); masks only against a kept
   list, each below 2^22; a scheme-2 index j below the kept list's length and used once in its
   record; every column read to its end. The encoder latches on anything else:
   a pad byte that is not zero, a length that is not 8 plus whole images. A body is at most
   8 + REC_ORDERS_CHUNK x REC_ORDER_IMAGE bytes, and the decoder writes every byte of it through
   recf_body_put, which bounds it by RECF_BODY_MAX before the write. */
#include "tagpu_recframe.h"

#define ORDERS_SCHEME 1                          /* a segment without RT_HOLD                  */
#define ORDERS_MATCH  2                          /* a segment with RT_HOLD                     */
#define ORDERS_HELD   65536u                     /* RECC_ORDERS_HELD: the arena's nodes at most */
#define ORDERS_SCAN   32u                        /* the kept nodes a node's match tries          */
#define NODE          REC_ORDER_IMAGE
#define NODE_DW       (REC_ORDER_IMAGE / 4)      /* 22: a mask's bits                          */
#define K_LIST        1u
#define K_WHOLE       2u
#define K_CHUNK       4u

enum { C_SLOT, C_KEY, C_MASK, C_D0, C_WHOLE = C_D0 + NODE_DW, C_IDX, ORDERS_COLS };
#define ORDERS_COLS1 C_IDX                       /* scheme 1's columns: all but idx            */
RECF_COLS_CHECK(orders, ORDERS_COLS);
_Static_assert(8 + REC_ORDERS_CHUNK * NODE <= RECF_BODY_MAX, "an RK_ORDERS the frame makes");

typedef struct Kept {
    uint32_t off, cap;          /* its place in the arena, in nodes                         */
    uint32_t n1;                /* the kept list's nodes + 1; 0: none                       */
} Kept;

typedef struct Orders {
    unsigned nslots;
    unsigned scheme, ncols;     /* the segment's: ORDERS_SCHEME or ORDERS_MATCH, its columns */
    Kept* kept;                 /* nslots * 2, (slot, list) at slot * 2 + list              */
    unsigned char* used;        /* REC_ORDERS_MAX: the kept nodes a scheme-2 record matched  */
    unsigned char* arena;       /* acap nodes, the first `held` of them placed              */
    uint32_t held, acap;
    int oom, unfit;
    uint32_t prev;              /* the previous record's slot in this iteration              */
    RecBytes col[ORDERS_COLS];  /* the encoder's columns, this block's                       */
    RecCol rd[ORDERS_COLS];     /* the decoder's                                             */
} Orders;

/* the arena's memory for `need` nodes (at most ORDERS_HELD): 0, no memory */
static int arena_grow(Orders* x, uint32_t need)
{
    uint32_t cap;
    unsigned char* p;
    if (need <= x->acap) return 1;
    cap = x->acap ? x->acap : 1024;
    while (cap < need) cap *= 2;
    if (cap > ORDERS_HELD) cap = ORDERS_HELD;
    p = (unsigned char*)realloc(x->arena, (size_t)cap * NODE);
    if (!p) return 0;
    x->arena = p;
    x->acap = cap;
    return 1;
}

/* After a record of the (slot, list) `l`: its nodes kept when it is the whole list and fits,
   else none (THE STATE). */
static void keep(Orders* x, Kept* l, unsigned total, unsigned first, unsigned k,
                 const unsigned char* nodes)
{
    if (first || k != total) {
        l->n1 = 0;
        return;
    }
    if (total > l->cap) {
        if (total > ORDERS_HELD - x->held) {
            l->n1 = 0;
            return;
        }
        if (!arena_grow(x, x->held + total)) {
            x->oom = 1;
            return;
        }
        l->off = x->held;
        l->cap = total;
        x->held += total;
    }
    if (total) memcpy(x->arena + (size_t)l->off * NODE, nodes, (size_t)total * NODE);
    l->n1 = total + 1;
}

static void* orders_make(const RecCodecParams* p)
{
    Orders* x = (Orders*)calloc(1, sizeof(Orders));
    if (!x) return NULL;
    x->nslots = p->nslots;
    x->scheme = p->hold ? ORDERS_MATCH : ORDERS_SCHEME;
    x->ncols = p->hold ? ORDERS_COLS : ORDERS_COLS1;
    x->kept = (Kept*)calloc((size_t)p->nslots * 2, sizeof(Kept));
    x->used = (unsigned char*)malloc(REC_ORDERS_MAX);
    if (!x->kept || !x->used) {
        free(x->kept);
        free(x->used);
        free(x);
        return NULL;
    }
    return x;
}

static void orders_drop(void* f)
{
    Orders* x = (Orders*)f;
    unsigned i;
    if (!x) return;
    for (i = 0; i < ORDERS_COLS; i++) rb_free(&x->col[i]);
    free(x->kept);
    free(x->used);
    free(x->arena);
    free(x);
}

static void orders_restart(void* f, const RecFrameWorld* w)
{
    Orders* x = (Orders*)f;
    unsigned i;
    (void)w;
    memset(x->kept, 0, (size_t)x->nslots * 2 * sizeof(Kept));
    x->held = 0;
    x->oom = x->unfit = 0;
    x->prev = 0;
    for (i = 0; i < ORDERS_COLS; i++) rb_clear(&x->col[i]);
}

#ifdef RECF_TEST_COPY
static int orders_copy(void* dst, const void* src)
{
    Orders* d = (Orders*)dst;
    const Orders* s = (const Orders*)src;
    if (s->held && !arena_grow(d, s->held)) return 0;
    memcpy(d->kept, s->kept, (size_t)s->nslots * 2 * sizeof(Kept));
    if (s->held) memcpy(d->arena, s->arena, (size_t)s->held * NODE);
    d->held = s->held;
    d->oom = s->oom;
    d->unfit = s->unfit;
    d->prev = s->prev;
    return 1;
}
#endif

static void orders_enc_iter(void* f, const RecFrameAt* a)
{
    Orders* x = (Orders*)f;
    unsigned i;
    if (!a->it)
        for (i = 0; i < ORDERS_COLS; i++) rb_clear(&x->col[i]);
    x->prev = 0;
}

/* node `now` against kept node `was`: its mask, and the dwords it names */
static void enc_mask(Orders* x, const unsigned char* now, const unsigned char* was)
{
    uint32_t m = 0;
    unsigned d;
    for (d = 0; d < NODE_DW; d++)
        if (memcmp(now + 4 * d, was + 4 * d, 4)) m |= 1u << d;
    rb_varint(&x->col[C_MASK], m);
    for (d = 0; d < NODE_DW; d++)
        if (m >> d & 1) rb_put(&x->col[C_D0 + d], now + 4 * d, 4);
}

/* A record in scheme 2: its key, then each node matched to a kept node or raw (THE BYTES). */
static void enc_matched(Orders* x, const Kept* l, unsigned list, unsigned total, unsigned first,
                        unsigned k, const unsigned char* nodes)
{
    unsigned kn = l->n1 ? l->n1 - 1 : 0, i, j, next = 0;
    if (!first && k == total && l->n1 == total + 1) {
        rb_u8(&x->col[C_KEY], list);
    } else {
        int chunk = first || k != total;
        rb_u8(&x->col[C_KEY], list | K_WHOLE | (chunk ? K_CHUNK : 0));
        rb_varint(&x->col[C_KEY], total);
        if (chunk) {
            rb_varint(&x->col[C_KEY], first);
            rb_varint(&x->col[C_KEY], k);
        }
    }
    for (i = 0; i < k; i++) {
        const unsigned char* now = nodes + (size_t)i * NODE;
        const unsigned char* was = NULL;
        for (j = next; j < kn && j < next + ORDERS_SCAN; j++) {
            was = x->arena + ((size_t)l->off + j) * NODE;
            if (was[4] == now[4] && !memcmp(was + 0x46, now + 0x46, 4)) break;
        }
        if (j < kn && j < next + ORDERS_SCAN) {
            rb_varint(&x->col[C_IDX], 1 + j);
            enc_mask(x, now, was);
            next = j + 1;
        } else {
            rb_varint(&x->col[C_IDX], 0);
            rb_put(&x->col[C_WHOLE], now, NODE);
        }
    }
}

static void orders_enc_rec(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                           unsigned len)
{
    Orders* x = (Orders*)f;
    unsigned slot, list, total, first, k, i, d;
    const unsigned char* nodes = b + 8;
    Kept* l;
    (void)a;
    (void)kind;
    if (x->unfit || x->oom) return;
    if (len < 8 || (len - 8) % NODE) {
        x->unfit = 1;
        return;
    }
    slot = get16(b);
    list = b[2];
    total = get16(b + 4);
    first = get16(b + 6);
    k = (len - 8) / NODE;
    if (b[3] || list > 1 || slot < 1 || slot >= x->nslots || total > REC_ORDERS_MAX ||
        k > REC_ORDERS_CHUNK || first + k > total || (total && !k)) {
        x->unfit = 1;
        return;
    }
    rb_varint(&x->col[C_SLOT], zz32(slot - x->prev));
    x->prev = slot;
    l = &x->kept[slot * 2 + list];
    if (x->scheme == ORDERS_MATCH) {
        enc_matched(x, l, list, total, first, k, nodes);
    } else if (!first && k == total && l->n1 == total + 1) {
        rb_u8(&x->col[C_KEY], list);
        for (i = 0; i < k; i++) {
            const unsigned char* now = nodes + (size_t)i * NODE;
            const unsigned char* was = x->arena + ((size_t)l->off + i) * NODE;
            uint32_t m = 0;
            for (d = 0; d < NODE_DW; d++)
                if (memcmp(now + 4 * d, was + 4 * d, 4)) m |= 1u << d;
            rb_varint(&x->col[C_MASK], m);
            for (d = 0; d < NODE_DW; d++)
                if (m >> d & 1) rb_put(&x->col[C_D0 + d], now + 4 * d, 4);
        }
    } else {
        int chunk = first || k != total;
        rb_u8(&x->col[C_KEY], list | K_WHOLE | (chunk ? K_CHUNK : 0));
        rb_varint(&x->col[C_KEY], total);
        if (chunk) {
            rb_varint(&x->col[C_KEY], first);
            rb_varint(&x->col[C_KEY], k);
        }
        rb_put(&x->col[C_WHOLE], nodes, (size_t)k * NODE);
    }
    keep(x, l, total, first, k, nodes);
}

/* a scheme-2 unit's death drops its slot's kept lists (THE STATE) */
static void orders_gone(void* f, unsigned slot)
{
    Orders* x = (Orders*)f;
    if (x->scheme != ORDERS_MATCH || slot >= x->nslots) return;
    x->kept[slot * 2].n1 = 0;
    x->kept[slot * 2 + 1].n1 = 0;
}

static void orders_enc_iter_end(void* f, const RecFrameAt* a)
{
    (void)f;
    (void)a;
}

static int orders_enc_write(void* f, RecBytes* sec)
{
    Orders* x = (Orders*)f;
    if (x->unfit || x->oom) return 0;
    return recf_write(sec, x->scheme, x->col, x->ncols);
}

static int orders_dec_open(void* f, const unsigned char* sec, size_t n, unsigned iterations,
                           char* why, unsigned cap)
{
    (void)iterations;
    Orders* x = (Orders*)f;
    return recf_open(sec, n, x->scheme, x->ncols, x->rd, "orders", why, cap);
}

static int orders_dec_iter(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)a;
    (void)why;
    (void)cap;
    ((Orders*)f)->prev = 0;
    return 1;
}

/* a node against kept node `was` onto the body: its mask, bounded, and the dwords it names */
static int dec_mask(Orders* x, const unsigned char* was, unsigned char* body, unsigned* len,
                    char* why, unsigned cap)
{
    RecCol* c = x->rd;
    uint32_t m, d;
    if (!recf_varint(&c[C_MASK], &m)) RECF_REFUSE("the orders frame's mask column cut short");
    if (m >> NODE_DW) RECF_REFUSE("an order node mask %lu past %u bits", (unsigned long)m, NODE_DW);
    for (d = 0; d < NODE_DW; d++) {
        RecCol* dc = &c[C_D0 + d];
        const unsigned char* src = was + 4 * d;
        if (m >> d & 1) {
            if (dc->end - dc->at < 4)
                RECF_REFUSE("the orders frame's dword column %lu cut short", (unsigned long)d);
            src = dc->p + dc->at;
            dc->at += 4;
        }
        if (!recf_body_put(body, len, src, 4)) RECF_REFUSE("an RK_ORDERS past %u bytes", RECF_BODY_MAX);
    }
    return 1;
}

static int orders_dec_rec(void* f, const RecFrameAt* a, unsigned kind, unsigned char* body,
                          unsigned* len, char* why, unsigned cap)
{
    Orders* x = (Orders*)f;
    RecCol* c = x->rd;
    uint32_t z, slot, total, first, k, kn, i;
    unsigned key;
    unsigned char head[8];
    Kept* l;
    (void)a;
    (void)kind;
    *len = 0;
    if (!recf_varint(&c[C_SLOT], &z)) RECF_REFUSE("the orders frame's slot column cut short");
    slot = x->prev + unzz32(z);
    if (slot < 1 || slot >= x->nslots) RECF_REFUSE("an RK_ORDERS for slot %lu", (unsigned long)slot);
    x->prev = slot;
    if (!recf_u8(&c[C_KEY], &key)) RECF_REFUSE("the orders frame's key column cut short");
    if (key & ~(K_LIST | K_WHOLE | K_CHUNK) || (key & K_CHUNK && !(key & K_WHOLE)))
        RECF_REFUSE("an RK_ORDERS key %u", key);
    l = &x->kept[slot * 2 + (key & K_LIST)];
    first = 0;
    if (key & K_WHOLE) {
        if (!recf_varint(&c[C_KEY], &total)) RECF_REFUSE("the orders frame's key column cut short");
        k = total;
        if (key & K_CHUNK && (!recf_varint(&c[C_KEY], &first) || !recf_varint(&c[C_KEY], &k)))
            RECF_REFUSE("the orders frame's key column cut short");
        if (total > REC_ORDERS_MAX || k > REC_ORDERS_CHUNK || k > total || first > total - k ||
            (total && !k))
            RECF_REFUSE("an RK_ORDERS of nodes [%lu, +%lu) of a list of %lu", (unsigned long)first,
                        (unsigned long)k, (unsigned long)total);
    } else {
        if (!l->n1)
            RECF_REFUSE("slot %lu list %u's RK_ORDERS coded against no kept list",
                        (unsigned long)slot, key & K_LIST);
        total = k = l->n1 - 1;
    }
    kn = l->n1 ? l->n1 - 1 : 0;
    head[0] = (unsigned char)slot;
    head[1] = (unsigned char)(slot >> 8);
    head[2] = (unsigned char)(key & K_LIST);
    head[3] = 0;
    head[4] = (unsigned char)total;
    head[5] = (unsigned char)(total >> 8);
    head[6] = (unsigned char)first;
    head[7] = (unsigned char)(first >> 8);
    if (!recf_body_put(body, len, head, 8)) RECF_REFUSE("an RK_ORDERS past %u bytes", RECF_BODY_MAX);
    if (x->scheme == ORDERS_MATCH) {
        memset(x->used, 0, kn);
        for (i = 0; i < k; i++) {
            RecCol* w = &c[C_WHOLE];
            uint32_t j1;
            if (!recf_varint(&c[C_IDX], &j1)) RECF_REFUSE("the orders frame's index column cut short");
            if (!j1) {
                if (NODE > w->end - w->at) RECF_REFUSE("the orders frame's node column cut short");
                if (!recf_body_put(body, len, w->p + w->at, NODE))
                    RECF_REFUSE("an RK_ORDERS past %u bytes", RECF_BODY_MAX);
                w->at += NODE;
                continue;
            }
            if (j1 > kn || x->used[j1 - 1])
                RECF_REFUSE("an order node matched to kept node %lu of %lu, or twice", (unsigned long)(j1 - 1),
                            (unsigned long)kn);
            x->used[j1 - 1] = 1;
            if (!dec_mask(x, x->arena + ((size_t)l->off + j1 - 1) * NODE, body, len, why, cap)) return 0;
        }
    } else if (key & K_WHOLE) {
        RecCol* w = &c[C_WHOLE];
        size_t n = (size_t)k * NODE;
        if (n > w->end - w->at) RECF_REFUSE("the orders frame's node column cut short");
        if (!recf_body_put(body, len, w->p + w->at, n))
            RECF_REFUSE("an RK_ORDERS past %u bytes", RECF_BODY_MAX);
        w->at += n;
    } else {
        for (i = 0; i < k; i++)
            if (!dec_mask(x, x->arena + ((size_t)l->off + i) * NODE, body, len, why, cap)) return 0;
    }
    keep(x, l, total, first, k, body + 8);
    if (x->oom) RECF_REFUSE("no memory for the orders frame's kept lists");
    return 1;
}

static int orders_dec_iter_end(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int orders_dec_close(void* f, char* why, unsigned cap)
{
    Orders* x = (Orders*)f;
    return recf_close(x->rd, x->ncols, "orders", why, cap);
}

const RecFrameOps recframe_orders = {
    "orders", RCS_ORDERS,
    orders_make, orders_drop, orders_restart, NULL, NULL, orders_gone,
#ifdef RECF_TEST_COPY
    orders_copy,
#endif
    orders_enc_iter, orders_enc_rec, orders_enc_iter_end, orders_enc_write,
    orders_dec_open, orders_dec_iter, orders_dec_rec, orders_dec_iter_end, orders_dec_close
};
