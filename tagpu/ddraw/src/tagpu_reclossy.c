/* tagpu_reclossy.c -- the lossy filter (tagpu_reclossy.h): the writer's exact iteration in, the
   iteration a skirmish recording's stream carries out.

   WHAT IT HOLDS. replay-hold.tsv names the bytes no reader on a playback host shows (RT_HOLD
   carries its masks): a unit image's held bits and a main list node's (+0x5C; the sub list is
   never held). The filter keeps D, the world a reader of the stream holds -- units, liveness and
   both lists of every slot -- built from the records it writes by the reader's own rules, never
   computed beside them, and writes, for each iteration:
   - a unit delta whose dwords are its slot's changes in D: the input's dwords that hold no held
     bit, and the held bits only where they are exact at a refresh. A held byte that changed alone
     is not written; one that changed beside a shown byte of its dword is kept at D's value;
   - an order list whose shown bytes equal D's but whose held bytes differ is dropped; any other
     list is written as it came, and D's becomes it;
   - every other record as it came.
   With a table of no rows nothing is held, every delta and list is written as it came, and the
   stream's records are the capture's; refreshes then only end blocks (tagpu_recwriter.c).

   THE INVARIANTS, each kept where the code below says:
   - EVERY VALUE WRITTEN IS EXACT (a lifetime). D's image of a slot is replaced whole by an
     RK_UNIT, a birth or a correction, and dropped at RK_UNIT_GONE with both its lists; a list is
     replaced whole whenever it is written. So a held byte in D is always a value the engine
     produced for that same unit, or that same list, since the last refresh: nothing is zeroed or
     made up (delta_filter, list_done).
   - EVERY SHOWN BYTE IS EXACT AT EVERY ITERATION. A delta's dwords with no held bit are the
     input's, a list is written whenever a shown byte of it differs from D's, and every record of
     another kind is copied (a correction among them: it is the writer's copy meeting the live
     state, and it reaches the file whatever the table holds).
   - D EQUALS THE WRITER'S COPY AT A REFRESH (an ordering). At a refresh every delta carries its
     held dwords too, each live slot no record named gets a delta to the copy's image, and each
     list that differs is written whole; the writer then checks reclossy_equal and abandons the
     file when it fails, so a snapshot, which always follows a refresh, is a world the stream
     reaches, and so is every takeover's start (tagpu_replay.c, tk_bound).
   - BOUNDS. Every slot is checked against nslots, a delta's bitmap against the image, a list's
     nodes against REC_ORDERS_MAX and its records against the list they continue.

   WHY A HELD VALUE NEVER REACHES A PLAYBACK HOST'S WORLD. The playing writes D into the engine at
   every iteration: every live unit's whole image, each byte replay-layout.tsv leaves a value and
   so every held byte (tagpu_replay.c, units_put through unit_put), and every node of both lists of
   every live unit (orders_put). So any engine write of a held byte on a puppet between two of
   those writes is overwritten by the next one, and what matters is only that no reader shows it in
   between. The engine writes of held bytes known to run on a puppet (DISASSEMBLED):
   - the unit destructor's reclaim credit read-modify-writes the killer's +0xD4
     (0x486CAD..0x486D1E) at a replayed death;
   - the death walk 0x489740 (from 0x486D75) ORs event 8 into a node's +0x4E through vtable
     0x4FD2C8 slot 0, 0x438870;
   - the shot receiver 0x49D270 writes the named weapon slot's pitch +0x18 (0x49D37C) and heading
     +0x16 (0x49D384) from the message, and the shot builders it calls read those values, the
     message's, never the image's;
   - the builder 0x49CC20, called from the receiver at 0x49D479, zeroes the slot's aim complete
     +0x8 (0x49CCA2: ebx 0 from 0x49CC2F, edi the slot from 0x49CC75);
   - the create's 0x489800 call (0x485C58, after 0x48A160 at 0x485C50) writes a fresh unit's slot
     targets and flags bit 4.
   No reader of any of them runs on a puppet in between: the panel reads only the ledger's last
   period, +0xCC/+0xD0/+0xE4/+0xE8, none of them held, +0x4E is read by the order controller and its handlers, and the
   slots' aim and target state by AutoAim and the handlers, owner code the unit tick skips for a
   type-3 owner (0x48ADC9..0x48ADD7, 0x48AEB5..0x48AECD). The Deathmatch respawn's writes of +0xBC
   and +0xD4 (0x465457..0x4654E7) run behind the defeat test, which the seat fill disarms on a
   puppet [INFERRED that they do not run]; the same rewrite covers them if they did.
   The filter writes no engine memory; the playing's puppet rows apply after every list it writes,
   whatever the stream held: ord_values copies a node's image and then applies replay-layout.tsv's
   puppet0 and puppet1 rows (+0x06 bit 1, +0x42 bits 0x400000 and 0x10000, +0x52), which keep the
   node destructor 0x43A1F0 from running owner code on a puppet (it reads +0x06 at 0x43A1F5 and
   tests bit 1 at 0x43A1FE before the handler call at 0x43A21A, reads +0x42 bit 0x10000 at
   0x43A2AB and +0x52 at 0x43A264..0x43A2A2, DISASSEMBLED).

   A REFRESH'S WRITES go where a reader takes them as images, before the iteration's events: the
   deltas in the run of unit records the iteration begins with, in slot order among the input's
   (the unit frame lists an iteration's deltas by ascending slot), and the lists before the
   iteration's first event or click. */
#include <stdlib.h>
#include <string.h>
#include "tagpu_reclossy.h"
#include "tagpu_recframe.h"

#define UNIT_DW  REC_UNIT_DWORDS
#define NODE_DW  (REC_ORDER_IMAGE / 4)

struct RecLossy {
    unsigned       nslots;
    uint32_t       hunit[UNIT_DW];      /* the held bits of each unit dword                    */
    uint32_t       hnode[NODE_DW];      /* ... and of each main list node dword                */
    unsigned char* units;               /* D: nslots images                                    */
    unsigned char* live;
    RecLossyList*  lists;               /* nslots * 2                                          */
    unsigned char* named;               /* scratch: a slot some unit record of the iteration names */
    unsigned*      touched;             /* ... the slots set, to clear them                    */
};

/* a set bit of a delta's bitmap is a dword of the image */
static int bit(const unsigned char* bits, unsigned i) { return bits[i >> 3] >> (i & 7) & 1; }

static uint32_t dw(const unsigned char* p, unsigned i)
{
    uint32_t v;
    memcpy(&v, p + 4 * i, 4);
    return v;
}

static void rec_put(RecBytes* out, unsigned kind, const void* pre, unsigned npre,
                    const void* body, unsigned nbody)
{
    static const unsigned char zero[4];
    unsigned len = npre + nbody;
    unsigned char h[4] = { (unsigned char)kind, (unsigned char)(kind >> 8), (unsigned char)len,
                           (unsigned char)(len >> 8) };
    rb_put(out, h, 4);
    rb_put(out, pre, npre);
    rb_put(out, body, nbody);
    rb_put(out, zero, ((4 + len + 3) & ~3u) - 4 - len);
}

RecLossy* reclossy_begin(const RecLossyParams* p)
{
    RecLossy* L;
    unsigned i;
    if (p->nslots < 2 || p->nslots > 0xFFFF) return NULL;
    L = (RecLossy*)calloc(1, sizeof *L);
    if (!L) return NULL;
    L->nslots = p->nslots;
    for (i = 0; i < UNIT_DW; i++) L->hunit[i] = dw(p->hold_unit, i);
    for (i = 0; i < NODE_DW; i++) L->hnode[i] = dw(p->hold_node, i);
    L->units = (unsigned char*)calloc(p->nslots, REC_UNIT_IMAGE);
    L->live = (unsigned char*)calloc(p->nslots, 1);
    L->lists = (RecLossyList*)calloc((size_t)p->nslots * 2, sizeof *L->lists);
    L->named = (unsigned char*)calloc(p->nslots, 1);
    L->touched = (unsigned*)malloc(p->nslots * sizeof *L->touched);
    if (!L->units || !L->live || !L->lists || !L->named || !L->touched) {
        reclossy_free(L);
        return NULL;
    }
    return L;
}

void reclossy_free(RecLossy* L)
{
    unsigned i;
    if (!L) return;
    for (i = 0; L->lists && i < L->nslots * 2; i++) free(L->lists[i].img);
    free(L->lists);
    free(L->units);
    free(L->live);
    free(L->named);
    free(L->touched);
    free(L);
}

static int list_set(RecLossyList* l, const unsigned char* img, unsigned n)
{
    if (n > l->cap) {
        unsigned char* q = (unsigned char*)realloc(l->img, (size_t)n * REC_ORDER_IMAGE);
        if (!q) return 0;
        l->img = q;
        l->cap = n;
    }
    if (img) memcpy(l->img, img, (size_t)n * REC_ORDER_IMAGE);
    l->n = n;
    return 1;
}

/* a delta of slot s from D's image to `to`, naming the dwords in `want`, into out and onto D */
static void delta_put(RecLossy* L, RecBytes* out, unsigned s, const uint32_t* to,
                      const unsigned char* want)
{
    unsigned char pre[REC_UNIT_DELTA_HEAD] = { 0 };
    uint32_t vals[UNIT_DW];
    unsigned i, k = 0;
    unsigned short slot = (unsigned short)s;
    unsigned char* d = L->units + (size_t)s * REC_UNIT_IMAGE;
    memcpy(pre, &slot, 2);
    for (i = 0; i < UNIT_DW; i++)
        if (bit(want, i)) {
            pre[2 + (i >> 3)] |= (unsigned char)(1u << (i & 7));
            vals[k++] = to[i];
            memcpy(d + 4 * i, &to[i], 4);
        }
    if (k) rec_put(out, RK_UNIT_DELTA, pre, REC_UNIT_DELTA_HEAD, vals, 4 * k);
}

/* An input delta of slot s, its bitmap `bits` and values `vals`, as the stream carries it.
   Between refreshes a dword's held bits keep D's value and its shown bits take the input's; at a
   refresh every dword is the writer's, the input's where it names one and `x`'s -- the copy's at
   the iteration's end -- for the rest unless a later record names the slot (`named`), whose
   image then replaces D's whole. The dwords written are the input's that hold no held bit and
   every other whose value changes in D: so with no held bit they are exactly the input's. */
static void delta_filter(RecLossy* L, RecBytes* out, unsigned s, const unsigned char* bits,
                         const unsigned char* vals, int refresh, const unsigned char* exact,
                         int named)
{
    const unsigned char* d = L->units + (size_t)s * REC_UNIT_IMAGE;
    unsigned char want[REC_UNIT_BITS] = { 0 };
    uint32_t to[UNIT_DW];
    unsigned i, k = 0;
    for (i = 0; i < UNIT_DW; i++) {
        uint32_t old = dw(d, i), v;
        if (bit(bits, i)) {
            v = dw(vals, k++);
            if (!refresh) v = (old & L->hunit[i]) | (v & ~L->hunit[i]);
            if (!L->hunit[i] || v != old) want[i >> 3] |= (unsigned char)(1u << (i & 7));
        } else {
            v = refresh && !named ? dw(exact, i) : old;
            if (v != old) want[i >> 3] |= (unsigned char)(1u << (i & 7));
        }
        to[i] = v;
    }
    delta_put(L, out, s, to, want);
}

/* At a refresh: slots [*from, upto) that are live, that no record of the iteration names, and
   whose D image differs from the copy's get a delta to it, in slot order. */
static void refresh_units(RecLossy* L, RecBytes* out, unsigned* from, unsigned upto, int refresh,
                          const RecLossyExact* x)
{
    unsigned s;
    if (refresh)
        for (s = *from; s < upto; s++) {
            const unsigned char* e = x->units + (size_t)s * REC_UNIT_IMAGE;
            const unsigned char* d = L->units + (size_t)s * REC_UNIT_IMAGE;
            unsigned char want[REC_UNIT_BITS] = { 0 };
            uint32_t to[UNIT_DW];
            unsigned i;
            if (!L->live[s] || L->named[s] || !memcmp(d, e, REC_UNIT_IMAGE)) continue;
            for (i = 0; i < UNIT_DW; i++) {
                to[i] = dw(e, i);
                if (to[i] != dw(d, i)) want[i >> 3] |= (unsigned char)(1u << (i & 7));
            }
            delta_put(L, out, s, to, want);
        }
    if (upto > *from) *from = upto;
}

/* At a refresh: every list that differs from the copy's, whole, so D's lists end as the copy's;
   its records as the capture's olist_put and the writer's list_put make them. The records' loop
   stays in this function: with no row held no list differs here, and the replay gate names a
   changed function its fixtures never reach (tools/replay/tareplay.py, landing). */
static int refresh_lists(RecLossy* L, RecBytes* out, int refresh, const RecLossyExact* x)
{
    unsigned i;
    if (!refresh) return 1;
    for (i = 2; i < L->nslots * 2; i++) {
        const RecLossyList* e = &x->lists[i];
        RecLossyList* d = &L->lists[i];
        unsigned first = 0;
        if (!L->live[i / 2] ||
            (d->n == e->n && (!e->n || !memcmp(d->img, e->img, (size_t)e->n * REC_ORDER_IMAGE))))
            continue;
        if (e->n > REC_ORDERS_MAX || !list_set(d, e->img, e->n)) return 0;
        do {
            unsigned char pre[8];
            unsigned short s = (unsigned short)(i / 2), len = (unsigned short)e->n;
            unsigned short f = (unsigned short)first;
            unsigned k = e->n - first < REC_ORDERS_CHUNK ? e->n - first : REC_ORDERS_CHUNK;
            memcpy(pre, &s, 2);
            pre[2] = (unsigned char)(i & 1);
            pre[3] = 0;
            memcpy(pre + 4, &len, 2);
            memcpy(pre + 6, &f, 2);
            rec_put(out, RK_ORDERS, pre, 8, e->img + (size_t)first * REC_ORDER_IMAGE,
                    k * REC_ORDER_IMAGE);
            first += k;
        } while (first < e->n);
    }
    return 1;
}

/* The order list that has begun arriving: its records stay in the input until its last, when
   list_done writes them or drops them. */
typedef struct Pend {
    unsigned slot, list, total, next;
    size_t   at;                /* its first record's offset in the input                     */
    int      shown, same;       /* every node so far equals D's in its shown bits; in every bit */
    int      correct;           /* a correction's list: written whatever the table holds       */
} Pend;

/* A list whose records [p->at, end) are all in: dropped when D holds its shown bytes and only its
   held bytes differ, else written as it came and taken into D. */
static int list_done(RecLossy* L, RecBytes* out, const Pend* p, const unsigned char* recs,
                     size_t end, int refresh)
{
    RecLossyList* d = &L->lists[p->slot * 2 + p->list];
    size_t at;
    if (!refresh && !p->correct && p->shown && !p->same) return 1;
    rb_put(out, recs + p->at, end - p->at);
    if (!list_set(d, NULL, p->total)) return 0;
    for (at = p->at; at < end; ) {
        unsigned short len, first;
        memcpy(&len, recs + at + 2, 2);
        memcpy(&first, recs + at + 4 + 6, 2);
        memcpy(d->img + (size_t)first * REC_ORDER_IMAGE, recs + at + 4 + 8, (size_t)len - 8);
        at += (4u + len + 3) & ~3u;
    }
    return 1;
}

/* An RK_ORDERS record: u16 slot, u8 list, u8 0, u16 the list's nodes, u16 the first one here,
   then whole node images (tagpu_recfmt.h). 1, 0 malformed. */
static int list_part(RecLossy* L, Pend* p, int* open, const unsigned char* b, unsigned len,
                     size_t at, int correct)
{
    unsigned short slot, total, first;
    unsigned list, k, i, j;
    const RecLossyList* d;
    if (len < 8 || (len - 8) % REC_ORDER_IMAGE) return 0;
    memcpy(&slot, b, 2);
    memcpy(&total, b + 4, 2);
    memcpy(&first, b + 6, 2);
    list = b[2];
    k = (len - 8) / REC_ORDER_IMAGE;
    if (list > 1 || b[3] || slot < 1 || slot >= L->nslots || !L->live[slot] ||
        total > REC_ORDERS_MAX || k > REC_ORDERS_CHUNK || first + k > total || (total && !k))
        return 0;
    if (!first) {
        if (*open) return 0;
        *open = 1;
        p->slot = slot;
        p->list = list;
        p->total = total;
        p->next = 0;
        p->at = at;
        p->correct = correct;
        p->shown = p->same = L->lists[slot * 2 + list].n == total;
    } else if (!*open || p->slot != slot || p->list != list || p->total != total ||
               p->next != first) {
        return 0;
    }
    d = &L->lists[slot * 2 + list];
    for (i = 0; i < k && p->shown; i++) {
        const unsigned char* a = b + 8 + (size_t)i * REC_ORDER_IMAGE;
        const unsigned char* o = d->img + (size_t)(first + i) * REC_ORDER_IMAGE;
        if (!memcmp(a, o, REC_ORDER_IMAGE)) continue;
        p->same = 0;
        for (j = 0; j < NODE_DW; j++) {
            uint32_t shown = list ? 0xFFFFFFFFu : ~L->hnode[j];
            if ((dw(a, j) ^ dw(o, j)) & shown) p->shown = 0;
        }
    }
    p->next = first + k;
    return 1;
}

int reclossy_iteration(RecLossy* L, const unsigned char* recs, size_t n, int refresh,
                       const RecLossyExact* x, RecBytes* out)
{
    size_t at = 0, end;
    unsigned ntouched = 0, ucur = 1, i;
    int units = 1, imaged = 1, open = 0, correct = 0, ok = 1;
    Pend p;
    memset(&p, 0, sizeof p);
    /* the frame, and every slot a unit record names: a slot named by an RK_UNIT or RK_UNIT_GONE
       gets its image from that record, and one that a delta names from the delta. A refusal
       here still reaches the loop at the end that clears the marks, so the next iteration
       starts from none. */
    for (end = 0; end < n; ) {
        unsigned short kind, len, s;
        if (n - end < 4) { ok = 0; break; }
        memcpy(&kind, recs + end, 2);
        memcpy(&len, recs + end + 2, 2);
        if (n - end - 4 < len || (!end && (kind != RK_TICK || len != 4))) { ok = 0; break; }
        if (kind == RK_UNIT || kind == RK_UNIT_GONE || kind == RK_UNIT_DELTA) {
            if (len < 2) { ok = 0; break; }
            memcpy(&s, recs + end + 4, 2);
            if (s < 1 || s >= L->nslots) { ok = 0; break; }
            if (!L->named[s]) L->touched[ntouched++] = s;
            L->named[s] |= kind == RK_UNIT_DELTA ? 1 : 2;
        }
        end += (4u + len + 3) & ~3u;
    }
    if (end != n) ok = 0;
    while (ok && at < n) {
        unsigned short kind, len, s = 0;
        const unsigned char* b = recs + at + 4;
        size_t size;
        memcpy(&kind, recs + at, 2);
        memcpy(&len, recs + at + 2, 2);
        size = (4u + len + 3) & ~3u;
        if (len >= 2) memcpy(&s, b, 2);
        if (open && kind != RK_ORDERS) { ok = 0; break; }
        if (units && at && kind != RK_UNIT && kind != RK_UNIT_GONE && kind != RK_UNIT_DELTA) {
            units = 0;
            refresh_units(L, out, &ucur, L->nslots, refresh, x);
        }
        if (imaged && (rec_section_of(kind) == RCS_EVENTS || kind == RK_CLICK)) {
            imaged = 0;
            if (!refresh_lists(L, out, refresh, x)) { ok = 0; break; }
        }
        if (units && at) refresh_units(L, out, &ucur, s, refresh, x);
        switch (kind) {
        case RK_UNIT:
            if (len != 4 + REC_UNIT_IMAGE) { ok = 0; break; }
            memcpy(L->units + (size_t)s * REC_UNIT_IMAGE, b + 4, REC_UNIT_IMAGE);
            L->live[s] = 1;
            rb_put(out, recs + at, size);
            break;
        case RK_UNIT_GONE:
            if (len != 4) { ok = 0; break; }
            L->live[s] = 0;
            L->lists[s * 2].n = L->lists[s * 2 + 1].n = 0;
            rb_put(out, recs + at, size);
            break;
        case RK_UNIT_DELTA: {
            unsigned k = 0;
            if (len < REC_UNIT_DELTA_HEAD || !L->live[s]) { ok = 0; break; }
            for (i = 0; i < REC_UNIT_BITS * 8; i++)
                if (bit(b + 2, i)) {
                    if (i >= UNIT_DW) { ok = 0; break; }
                    k++;
                }
            if (!ok || len != REC_UNIT_DELTA_HEAD + 4 * k) { ok = 0; break; }
            delta_filter(L, out, s, b + 2, b + REC_UNIT_DELTA_HEAD, refresh,
                         x->units + (size_t)s * REC_UNIT_IMAGE, L->named[s] & 2);
            break;
        }
        case RK_ORDERS:
            if (!list_part(L, &p, &open, b, len, at, correct)) { ok = 0; break; }
            if (p.next == p.total) {
                open = 0;
                if (!list_done(L, out, &p, recs, at + size, refresh)) ok = 0;
            }
            break;
        default:
            rb_put(out, recs + at, size);
            break;
        }
        /* RK_CORRECT introduces the next record, or the next list's records */
        if (kind == RK_CORRECT) correct = 1;
        else if (!open) correct = 0;
        at += size;
    }
    if (ok && open) ok = 0;
    if (ok && units) refresh_units(L, out, &ucur, L->nslots, refresh, x);
    if (ok && imaged && !refresh_lists(L, out, refresh, x)) ok = 0;
    for (i = 0; i < ntouched; i++) L->named[L->touched[i]] = 0;
    return ok && !out->oom;
}

int reclossy_equal(const RecLossy* L, const RecLossyExact* x)
{
    unsigned s, k;
    for (s = 1; s < L->nslots; s++) {
        if (!L->live[s] != !x->live[s]) return 0;
        if (L->live[s] && memcmp(L->units + (size_t)s * REC_UNIT_IMAGE,
                                 x->units + (size_t)s * REC_UNIT_IMAGE, REC_UNIT_IMAGE))
            return 0;
        for (k = 0; k < 2; k++) {
            const RecLossyList* d = &L->lists[s * 2 + k];
            const RecLossyList* e = &x->lists[s * 2 + k];
            if (d->n != e->n || (e->n && memcmp(d->img, e->img, (size_t)e->n * REC_ORDER_IMAGE)))
                return 0;
        }
    }
    return 1;
}

/* the interval of the refresh grid that holds GameTime t: 1 to interval is the first */
static unsigned long long refresh_bin(unsigned t, unsigned interval)
{
    return ((unsigned long long)t + interval - 1) / interval;
}

int reclossy_is_refresh(unsigned n, unsigned tick, unsigned prev, unsigned interval)
{
    return n <= 2 || (interval && refresh_bin(tick, interval) != refresh_bin(prev, interval));
}

int reclossy_hold_parse(RecLossyHold* h, const void* p, unsigned n, char* why, unsigned cap)
{
    const unsigned char* b = (const unsigned char*)p;
    unsigned i;
    if (n != REC_HOLD_BYTES) RECF_REFUSE("an RT_HOLD of %u bytes, not %u", n, REC_HOLD_BYTES);
    memcpy(&h->refresh, b, 4);
    memcpy(&h->flags, b + 4, 4);
    memcpy(h->sha, b + 8, 32);
    memcpy(h->unit, b + 40, REC_UNIT_IMAGE);
    memcpy(h->node, b + 40 + REC_UNIT_IMAGE, REC_ORDER_IMAGE);
    if (h->flags & ~(RECL_CELLS | RECL_SPRAYS)) RECF_REFUSE("RT_HOLD's flags 0x%X", h->flags);
    if (h->refresh < REC_REFRESH_MIN || h->refresh > REC_REFRESH_TICKS)
        RECF_REFUSE("RT_HOLD's refresh interval %u", h->refresh);
    for (i = REC_HOLD_POS_AT; i < REC_HOLD_POS_END; i++)
        if (h->unit[i]) RECF_REFUSE("RT_HOLD holds the unit position's byte +0x%X", i);
    return 1;
}

unsigned reclossy_refresh_at(const RecLossyBlock* b, unsigned n, unsigned first, unsigned T)
{
    unsigned i, r;
    if (T < first) return 0;
    r = first;
    for (i = 0; i < n; i++)
        if (b[i].refresh && b[i].last <= T && b[i].last > r) r = b[i].last;
    return r;
}
