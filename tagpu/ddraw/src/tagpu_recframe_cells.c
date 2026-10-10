/* tagpu_recframe_cells.c -- the cells frame (tagpu_recfmt.h, RCS_CELLS; tagpu_recframe.h says what
   a frame is and what it shares): every RK_CELLS of a sectioned block, coded against a copy of the
   feature grid and against the last iteration's tick input.

   An RK_CELLS is an IMAGE when no RCS_EVENTS record came earlier in its iteration, else TICK
   INPUT: the reader's rule (RecFrameAt.events), under which the cells after an event are the
   feature tick's.

   ENGINE FACTS.
   - The cell, 13 bytes (exe-reverse-engineering.md, *The cell*, DISASSEMBLED from 0x481550 and
     the functions that table cites), split into the six fields this frame codes: [0, 2) slot A,
     [2, 4) slot B, [4, 8) the heights and the metal, [8, 10) the feature def, [10, 12) a
     footprint cell's way back to its anchor or an anchor's record or damage, [12, 13) the flags.
     Slot A empties when the unit filed there leaves the cell, which the sym 0x40 codes in one
     byte [INFERRED: the stamp files a ground unit in slot A].
   - The recorder's cells_tick (tagpu_recorder.c) writes the feature tick's changed cells as tick
     input and leaves its shadow as it was, so the next iteration's images repeat that tick input.
     MEASURED 2026-10-09/10 on format-9 recordings, after each run's first iteration: the image
     list equals the previous iteration's tick input record for record in 7,399 of 7,401
     iterations (Core Prime Industrial Area), 3,956 of 3,985 (Town & Country) and 5,757 of 5,904
     (an air war); MEASURED 2026-10-10 on the same four rewritten as format 10, the tick input is
     at most 1,024 cells an iteration (the air war).

   THE BYTES. Scheme 1, nine columns:
     flag      one u8 at an iteration's first image: 1, its images are T record for record, and
               no other column holds them; 0, they are coded as below
     rec       per coded record: varint zz32(p - e), p = z * W + x its first cell's index and e
               the end (p + cells) of the record before it in the same list -- the iteration's
               images or its tick input -- 0 for the first; then varint cells - 1
     sym       per coded cell, against its copy in G when known, else 13 zero bytes: bits 0..5 the
               fields that differ; 0x40, only slot A differs and it is now 0
     f0..f5    the differing fields' bytes, a column per field, in cell order
   THE STATE, all of it cleared at every reset (tagpu_recframe.h):
     G         the grid's copy, W * H cells, each with a known bit; an image writes its cells
     T, T'     the last iteration's tick input and this one's: each record's first cell, its cell
               count and its cells. T takes T' at an iteration's end, empty when T' passed
               CELLS_HELD cells; the rule reads the records alone, so every walk keeps the same T.
     the encoder's latch: a record it cannot code (a bad pad, length or run) declines the block
     after its iterations.
   THE BOUNDS. A coded record's run is 1 <= cells <= REC_CELLS_MAX on one row, x + cells <= W and
   z < H, its length 8 + 13 cells and its pad 0; a sym is 0..0x3F or 0x40; a flag 0 or 1, and with
   1 the iteration takes exactly T's records. Each column is read to its end.

   MEMORY: 13.125 bytes a grid cell, G and its known bits (5.5 MB on a 642 x 648 grid), and T and
   T', CELLS_HELD cells each at 19 bytes a cell (0.6 MB). */
#include "tagpu_recframe.h"

#define CELLS_SCHEME 1
#define CELLS_COLS   9
#define CELLS_HELD   (4u * REC_CELLS_MAX)   /* T's cells at most: past it, T is empty           */
#define CB           REC_CELL_BYTES
#define SYM_A_EMPTY  0x40

enum { COL_FLAG, COL_REC, COL_SYM, COL_F0 };
RECF_COLS_CHECK(cells, CELLS_COLS);

/* the fields: field f is the bytes [FIELD[f], FIELD[f + 1]) of a cell */
static const unsigned char FIELD[7] = { 0, 2, 4, 8, 10, 12, CB };

typedef struct Tick {
    uint32_t*      p;           /* each record's first cell                          */
    uint16_t*      n;           /* its cells                                         */
    unsigned char* cell;        /* the records' cells, in order                      */
    unsigned       nrec, ncell;
    int            over;        /* it passed CELLS_HELD: T is empty after it         */
} Tick;

typedef struct Cells {
    unsigned       w, h;
    size_t         wh;
    unsigned char* g;           /* G: wh cells                                       */
    unsigned char* known;       /* its known bits                                    */
    Tick           t[2];        /* t[cur] is T, t[cur ^ 1] T'                        */
    unsigned       cur;
    /* the iteration */
    int            imaged;      /* an image came                                     */
    int            flag;        /* its flag: the images are T                        */
    int            pending;     /* the encoder's: every image so far is T's next     */
    unsigned       tnext;       /* T's next record                                   */
    size_t         tcell;       /* and its first cell in T's cells                   */
    uint32_t       e[2];        /* each list's last end: the images, the tick input  */
    int            bad;         /* the encoder's latch                               */
    RecBytes       col[CELLS_COLS];
    RecCol         rd[CELLS_COLS];
} Cells;

static const unsigned char ZERO[CB];

static int known_at(const Cells* x, size_t q) { return x->known[q >> 3] >> (q & 7) & 1; }

static void tick_empty(Tick* t)
{
    t->nrec = t->ncell = 0;
    t->over = 0;
}

/* a record into T', or T' over its bound */
static void tick_hold(Cells* x, uint32_t p, unsigned n, const unsigned char* cells)
{
    Tick* t = &x->t[x->cur ^ 1];
    if (t->over) return;
    if (n > CELLS_HELD - t->ncell) {
        t->over = 1;
        return;
    }
    t->p[t->nrec] = p;
    t->n[t->nrec] = (uint16_t)n;
    memcpy(t->cell + (size_t)t->ncell * CB, cells, (size_t)n * CB);
    t->nrec++;
    t->ncell += n;
}

/* T takes T', and an empty T' starts */
static void tick_turn(Cells* x)
{
    Tick* t = &x->t[x->cur ^ 1];
    if (t->over) tick_empty(t);
    x->cur ^= 1;
    tick_empty(&x->t[x->cur ^ 1]);
}

/* an image's cells into G */
static void grid_put(Cells* x, uint32_t p, unsigned n, const unsigned char* cells)
{
    size_t q;
    memcpy(x->g + (size_t)p * CB, cells, (size_t)n * CB);
    for (q = p; q < (size_t)p + n; q++) x->known[q >> 3] |= (unsigned char)(1u << (q & 7));
}

static void cells_drop(void* f)
{
    Cells* x = (Cells*)f;
    unsigned i;
    if (!x) return;
    free(x->g);
    free(x->known);
    for (i = 0; i < 2; i++) {
        free(x->t[i].p);
        free(x->t[i].n);
        free(x->t[i].cell);
    }
    for (i = 0; i < CELLS_COLS; i++) rb_free(&x->col[i]);
    free(x);
}

static void* cells_make(const RecCodecParams* p)
{
    Cells* x;
    unsigned i;
    int ok;
    if (p->gw < 1 || p->gw > 0xFFFF || p->gh < 1 || p->gh > 0xFFFF ||
        (size_t)p->gw * p->gh > RECC_GRID_MAX)
        return NULL;
    x = (Cells*)calloc(1, sizeof *x);
    if (!x) return NULL;
    x->w = p->gw;
    x->h = p->gh;
    x->wh = (size_t)p->gw * p->gh;
    x->g = (unsigned char*)malloc(x->wh * CB);
    x->known = (unsigned char*)calloc((x->wh + 7) / 8, 1);
    ok = x->g && x->known;
    for (i = 0; i < 2; i++) {
        x->t[i].p = (uint32_t*)malloc(CELLS_HELD * sizeof *x->t[i].p);
        x->t[i].n = (uint16_t*)malloc(CELLS_HELD * sizeof *x->t[i].n);
        x->t[i].cell = (unsigned char*)malloc((size_t)CELLS_HELD * CB);
        ok &= x->t[i].p && x->t[i].n && x->t[i].cell;
    }
    if (!ok) {
        cells_drop(x);
        return NULL;
    }
    return x;
}

static void cells_restart(void* f)
{
    Cells* x = (Cells*)f;
    unsigned i;
    memset(x->known, 0, (x->wh + 7) / 8);
    tick_empty(&x->t[0]);
    tick_empty(&x->t[1]);
    x->cur = 0;
    x->imaged = x->flag = x->pending = 0;
    x->tnext = 0;
    x->tcell = 0;
    x->e[0] = x->e[1] = 0;
    x->bad = 0;
    for (i = 0; i < CELLS_COLS; i++) rb_clear(&x->col[i]);
}

static int cells_copy(void* dst, const void* src)
{
    Cells* d = (Cells*)dst;
    const Cells* s = (const Cells*)src;
    unsigned i;
    memcpy(d->g, s->g, s->wh * CB);
    memcpy(d->known, s->known, (s->wh + 7) / 8);
    for (i = 0; i < 2; i++) {
        memcpy(d->t[i].p, s->t[i].p, s->t[i].nrec * sizeof *s->t[i].p);
        memcpy(d->t[i].n, s->t[i].n, s->t[i].nrec * sizeof *s->t[i].n);
        memcpy(d->t[i].cell, s->t[i].cell, (size_t)s->t[i].ncell * CB);
        d->t[i].nrec = s->t[i].nrec;
        d->t[i].ncell = s->t[i].ncell;
        d->t[i].over = s->t[i].over;
    }
    d->cur = s->cur;
    d->imaged = s->imaged;
    d->flag = s->flag;
    d->pending = s->pending;
    d->tnext = s->tnext;
    d->tcell = s->tcell;
    d->e[0] = s->e[0];
    d->e[1] = s->e[1];
    d->bad = s->bad;
    return 1;
}

static void iter_start(Cells* x)
{
    x->imaged = x->flag = x->pending = 0;
    x->tnext = 0;
    x->tcell = 0;
    x->e[0] = x->e[1] = 0;
    tick_empty(&x->t[x->cur ^ 1]);
}

/* ---- the encoder ---------------------------------------------------------------------------- */

/* one record's run and cells into the columns, `list` 0 the images and 1 the tick input */
static void enc_code(Cells* x, unsigned list, uint32_t p, unsigned n, const unsigned char* cells)
{
    unsigned k, f;
    rb_varint(&x->col[COL_REC], zz32(p - x->e[list]));
    rb_varint(&x->col[COL_REC], n - 1);
    x->e[list] = p + n;
    for (k = 0; k < n; k++) {
        size_t q = (size_t)p + k;
        const unsigned char* c = cells + (size_t)k * CB;
        const unsigned char* ref = known_at(x, q) ? x->g + q * CB : ZERO;
        unsigned m = 0;
        for (f = 0; f < 6; f++)
            if (memcmp(c + FIELD[f], ref + FIELD[f], FIELD[f + 1] - FIELD[f])) m |= 1u << f;
        if (m == 1 && !c[0] && !c[1]) {
            rb_u8(&x->col[COL_SYM], SYM_A_EMPTY);
            continue;
        }
        rb_u8(&x->col[COL_SYM], m);
        for (f = 0; f < 6; f++)
            if (m >> f & 1) rb_put(&x->col[COL_F0 + f], c + FIELD[f], FIELD[f + 1] - FIELD[f]);
    }
}

/* The images so far, T's first tnext records, decided: `same`, they are all of T (flag 1), else
   each is coded. Either way they reach G in order. */
static void enc_settle(Cells* x, int same)
{
    const Tick* t = &x->t[x->cur];
    unsigned i;
    size_t at = 0;
    x->pending = 0;
    rb_u8(&x->col[COL_FLAG], same);
    for (i = 0; i < x->tnext; i++) {
        const unsigned char* cells = t->cell + at * CB;
        if (!same) enc_code(x, 0, t->p[i], t->n[i], cells);
        grid_put(x, t->p[i], t->n[i], cells);
        at += t->n[i];
    }
}

static void cells_enc_iter(void* f, const RecFrameAt* a)
{
    Cells* x = (Cells*)f;
    unsigned i;
    if (!a->it)
        for (i = 0; i < CELLS_COLS; i++) rb_clear(&x->col[i]);
    iter_start(x);
}

static void cells_enc_rec(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                          unsigned len)
{
    Cells* x = (Cells*)f;
    unsigned cx, cz, n;
    uint32_t p;
    const unsigned char* cells = b + 8;
    (void)kind;
    if (x->bad) return;
    if (len < 8) {
        x->bad = 1;
        return;
    }
    cx = get16(b);
    cz = get16(b + 2);
    n = get16(b + 4);
    if (get16(b + 6) || n < 1 || n > REC_CELLS_MAX || len != 8 + n * CB || cx + n > x->w ||
        cz >= x->h) {
        x->bad = 1;
        return;
    }
    p = (uint32_t)(cz * x->w + cx);
    if (!a->events) {
        if (!x->imaged) {
            x->imaged = 1;
            x->pending = 1;
        }
        if (x->pending) {
            const Tick* t = &x->t[x->cur];
            if (x->tnext < t->nrec && t->p[x->tnext] == p && t->n[x->tnext] == n &&
                !memcmp(t->cell + x->tcell * CB, cells, (size_t)n * CB)) {
                x->tnext++;
                x->tcell += n;
                return;
            }
            enc_settle(x, 0);
        }
        enc_code(x, 0, p, n, cells);
        grid_put(x, p, n, cells);
        return;
    }
    if (x->pending) enc_settle(x, x->tnext == x->t[x->cur].nrec);
    enc_code(x, 1, p, n, cells);
    tick_hold(x, p, n, cells);
}

static void cells_enc_iter_end(void* f, const RecFrameAt* a)
{
    Cells* x = (Cells*)f;
    (void)a;
    if (x->bad) return;
    if (x->pending) enc_settle(x, x->tnext == x->t[x->cur].nrec);
    tick_turn(x);
}

static int cells_enc_write(void* f, RecBytes* sec)
{
    Cells* x = (Cells*)f;
    return !x->bad && recf_write(sec, CELLS_SCHEME, x->col, CELLS_COLS);
}

/* ---- the decoder ---------------------------------------------------------------------------- */

static int cells_dec_open(void* f, const unsigned char* sec, size_t n, unsigned iterations,
                          char* why, unsigned cap)
{
    (void)iterations;
    return recf_open(sec, n, CELLS_SCHEME, CELLS_COLS, ((Cells*)f)->rd, "cells", why, cap);
}

static int cells_dec_iter(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)a;
    (void)why;
    (void)cap;
    iter_start((Cells*)f);
    return 1;
}

/* a record's head, x, z, cells and a zero pad, as the body's first bytes */
static int body_head(unsigned char* body, unsigned* len, uint32_t p, unsigned w, unsigned n)
{
    unsigned char h[8];
    unsigned cx = p % w, cz = p / w;
    h[0] = (unsigned char)cx;
    h[1] = (unsigned char)(cx >> 8);
    h[2] = (unsigned char)cz;
    h[3] = (unsigned char)(cz >> 8);
    h[4] = (unsigned char)n;
    h[5] = (unsigned char)(n >> 8);
    h[6] = h[7] = 0;
    *len = 0;
    return recf_body_put(body, len, h, 8);
}

/* one coded record of `list` into body: its run, bounded, then each cell from its sym */
static int dec_code(Cells* x, unsigned list, unsigned char* body, unsigned* len, uint32_t* pp,
                    unsigned* pn, char* why, unsigned cap)
{
    uint32_t r, m1, p;
    unsigned n, k, f, s;
    if (!recf_varint(&x->rd[COL_REC], &r) || !recf_varint(&x->rd[COL_REC], &m1))
        RECF_REFUSE("the cells frame's runs cut short");
    p = x->e[list] + unzz32(r);
    if (p >= x->wh) RECF_REFUSE("an RK_CELLS at cell %lu of a %lu-cell grid", (unsigned long)p, (unsigned long)x->wh);
    if (m1 >= REC_CELLS_MAX) RECF_REFUSE("an RK_CELLS of %lu cells", (unsigned long)m1 + 1);
    n = m1 + 1;
    if (p % x->w + n > x->w) RECF_REFUSE("an RK_CELLS of %u cells from column %u of %u", n, (unsigned)(p % x->w), x->w);
    x->e[list] = p + n;
    if (!body_head(body, len, p, x->w, n)) RECF_REFUSE("an RK_CELLS past %u bytes", RECF_BODY_MAX);
    for (k = 0; k < n; k++) {
        size_t q = (size_t)p + k;
        unsigned char c[CB];
        if (!recf_u8(&x->rd[COL_SYM], &s)) RECF_REFUSE("the cells frame's syms cut short");
        memcpy(c, known_at(x, q) ? x->g + q * CB : ZERO, CB);
        if (s == SYM_A_EMPTY) {
            c[0] = c[1] = 0;
        } else {
            if (s > 0x3F) RECF_REFUSE("a cell's sym %#x", s);
            for (f = 0; f < 6; f++)
                if (s >> f & 1 && !recf_bytes(&x->rd[COL_F0 + f], c + FIELD[f], FIELD[f + 1] - FIELD[f]))
                    RECF_REFUSE("the cells frame's field %u cut short", f);
        }
        if (!recf_body_put(body, len, c, CB)) RECF_REFUSE("an RK_CELLS past %u bytes", RECF_BODY_MAX);
    }
    *pp = p;
    *pn = n;
    return 1;
}

static int cells_dec_rec(void* f, const RecFrameAt* a, unsigned kind, unsigned char* body,
                         unsigned* len, char* why, unsigned cap)
{
    Cells* x = (Cells*)f;
    uint32_t p;
    unsigned n;
    (void)kind;
    if (!a->events) {
        if (!x->imaged) {
            unsigned v;
            x->imaged = 1;
            if (!recf_u8(&x->rd[COL_FLAG], &v) || v > 1)
                RECF_REFUSE("iteration %u: the cells frame's flag cut short or past 1", a->it);
            x->flag = (int)v;
        }
        if (x->flag) {
            const Tick* t = &x->t[x->cur];
            if (x->tnext >= t->nrec)
                RECF_REFUSE("iteration %u: an image past the last tick input's %u records", a->it, t->nrec);
            p = t->p[x->tnext];
            n = t->n[x->tnext];
            if (!body_head(body, len, p, x->w, n) ||
                !recf_body_put(body, len, t->cell + x->tcell * CB, (size_t)n * CB))
                RECF_REFUSE("an RK_CELLS past %u bytes", RECF_BODY_MAX);
            x->tnext++;
            x->tcell += n;
        } else if (!dec_code(x, 0, body, len, &p, &n, why, cap)) {
            return 0;
        }
        grid_put(x, p, n, body + 8);
    } else {
        if (!dec_code(x, 1, body, len, &p, &n, why, cap)) return 0;
        tick_hold(x, p, n, body + 8);
    }
    return 1;
}

static int cells_dec_iter_end(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    Cells* x = (Cells*)f;
    if (x->flag && x->tnext != x->t[x->cur].nrec)
        RECF_REFUSE("iteration %u: its images are %u of the last tick input's %u records", a->it,
                    x->tnext, x->t[x->cur].nrec);
    tick_turn(x);
    return 1;
}

static int cells_dec_close(void* f, char* why, unsigned cap)
{
    return recf_close(((Cells*)f)->rd, CELLS_COLS, "cells", why, cap);
}

const RecFrameOps recframe_cells = {
    "cells", RCS_CELLS,
    cells_make, cells_drop, cells_restart, cells_copy,
    cells_enc_iter, cells_enc_rec, cells_enc_iter_end, cells_enc_write,
    cells_dec_open, cells_dec_iter, cells_dec_rec, cells_dec_iter_end, cells_dec_close
};
