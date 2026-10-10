/* tagpu_recframe_orders.c -- the orders frame (tagpu_recfmt.h, RCS_ORDERS; tagpu_recframe.h says
   what a frame is and what it shares): every RK_ORDERS of a sectioned block.

   THE BYTES. Scheme 0, one column: for each record of its kinds in record order, a varint length
   and the body verbatim.
   THE STATE. The block's column, and the decoder's cursor in it: nothing crosses a block, and
   all of it is cleared at every reset.
   THE BOUNDS. A length is at most RECF_BODY_MAX and the column's bytes left; the column is read
   to its end. */
#include "tagpu_recframe.h"

#define ORDERS_SCHEME 0
#define ORDERS_COLS   1

typedef struct Orders {
    RecBytes col;               /* the encoder's column, this block's */
    RecCol   rd;                /* the decoder's                      */
} Orders;

static void* orders_make(const RecCodecParams* p)
{
    (void)p;
    return calloc(1, sizeof(Orders));
}

static void orders_drop(void* f)
{
    Orders* x = (Orders*)f;
    if (!x) return;
    rb_free(&x->col);
    free(x);
}

static void orders_restart(void* f) { rb_clear(&((Orders*)f)->col); }

static int orders_copy(void* dst, const void* src)
{
    (void)dst;
    (void)src;
    return 1;
}

static void orders_enc_iter(void* f, const RecFrameAt* a)
{
    if (!a->it) rb_clear(&((Orders*)f)->col);
}

static void orders_enc_rec(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                           unsigned len)
{
    Orders* x = (Orders*)f;
    (void)a;
    (void)kind;
    rb_varint(&x->col, len);
    rb_put(&x->col, b, len);
}

static void orders_enc_iter_end(void* f, const RecFrameAt* a)
{
    (void)f;
    (void)a;
}

static int orders_enc_write(void* f, RecBytes* sec)
{
    return recf_write(sec, ORDERS_SCHEME, &((Orders*)f)->col, ORDERS_COLS);
}

static int orders_dec_open(void* f, const unsigned char* sec, size_t n, unsigned iterations,
                           char* why, unsigned cap)
{
    (void)iterations;
    return recf_open(sec, n, ORDERS_SCHEME, ORDERS_COLS, &((Orders*)f)->rd, "orders", why, cap);
}

static int orders_dec_iter(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int orders_dec_rec(void* f, const RecFrameAt* a, unsigned kind, unsigned char* body,
                          unsigned* len, char* why, unsigned cap)
{
    Orders* x = (Orders*)f;
    uint32_t n;
    (void)a;
    if (!recf_varint(&x->rd, &n) || n > RECF_BODY_MAX || !recf_bytes(&x->rd, body, n))
        RECF_REFUSE("an RK_ORDERS (kind %u) cut short or past %u bytes in the orders frame",
                    kind, RECF_BODY_MAX);
    *len = n;
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
    return recf_close(&((Orders*)f)->rd, ORDERS_COLS, "orders", why, cap);
}

const RecFrameOps recframe_orders = {
    "orders", RCS_ORDERS,
    orders_make, orders_drop, orders_restart, orders_copy,
    orders_enc_iter, orders_enc_rec, orders_enc_iter_end, orders_enc_write,
    orders_dec_open, orders_dec_iter, orders_dec_rec, orders_dec_iter_end, orders_dec_close
};
