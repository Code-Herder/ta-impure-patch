/* tagpu_recframe_cells.c -- the cells frame (tagpu_recfmt.h, RCS_CELLS; tagpu_recframe.h says what
   a frame is and what it shares): every RK_CELLS of a sectioned block.

   THE BYTES. Scheme 0, one column: for each record of its kinds in record order, a varint length
   and the body verbatim.
   THE STATE. The block's column, and the decoder's cursor in it: nothing crosses a block, and
   all of it is cleared at every reset.
   THE BOUNDS. A length is at most RECF_BODY_MAX and the column's bytes left; the column is read
   to its end. */
#include "tagpu_recframe.h"

#define CELLS_SCHEME 0
#define CELLS_COLS   1

typedef struct Cells {
    RecBytes col;               /* the encoder's column, this block's */
    RecCol   rd;                /* the decoder's                      */
} Cells;

static void* cells_make(const RecCodecParams* p)
{
    (void)p;
    return calloc(1, sizeof(Cells));
}

static void cells_drop(void* f)
{
    Cells* x = (Cells*)f;
    if (!x) return;
    rb_free(&x->col);
    free(x);
}

static void cells_restart(void* f) { rb_clear(&((Cells*)f)->col); }

static int cells_copy(void* dst, const void* src)
{
    (void)dst;
    (void)src;
    return 1;
}

static void cells_enc_iter(void* f, const RecFrameAt* a)
{
    if (!a->it) rb_clear(&((Cells*)f)->col);
}

static void cells_enc_rec(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                          unsigned len)
{
    Cells* x = (Cells*)f;
    (void)a;
    (void)kind;
    rb_varint(&x->col, len);
    rb_put(&x->col, b, len);
}

static void cells_enc_iter_end(void* f, const RecFrameAt* a)
{
    (void)f;
    (void)a;
}

static int cells_enc_write(void* f, RecBytes* sec)
{
    return recf_write(sec, CELLS_SCHEME, &((Cells*)f)->col, CELLS_COLS);
}

static int cells_dec_open(void* f, const unsigned char* sec, size_t n, unsigned iterations,
                          char* why, unsigned cap)
{
    (void)iterations;
    return recf_open(sec, n, CELLS_SCHEME, CELLS_COLS, &((Cells*)f)->rd, "cells", why, cap);
}

static int cells_dec_iter(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int cells_dec_rec(void* f, const RecFrameAt* a, unsigned kind, unsigned char* body,
                         unsigned* len, char* why, unsigned cap)
{
    Cells* x = (Cells*)f;
    uint32_t n;
    (void)a;
    if (!recf_varint(&x->rd, &n) || n > RECF_BODY_MAX || !recf_bytes(&x->rd, body, n))
        RECF_REFUSE("an RK_CELLS (kind %u) cut short or past %u bytes in the cells frame",
                    kind, RECF_BODY_MAX);
    *len = n;
    return 1;
}

static int cells_dec_iter_end(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int cells_dec_close(void* f, char* why, unsigned cap)
{
    return recf_close(&((Cells*)f)->rd, CELLS_COLS, "cells", why, cap);
}

const RecFrameOps recframe_cells = {
    "cells", RCS_CELLS,
    cells_make, cells_drop, cells_restart, cells_copy,
    cells_enc_iter, cells_enc_rec, cells_enc_iter_end, cells_enc_write,
    cells_dec_open, cells_dec_iter, cells_dec_rec, cells_dec_iter_end, cells_dec_close
};
