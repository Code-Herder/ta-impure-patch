/* tagpu_recframe_events.c -- the events frame (tagpu_recfmt.h, RCS_EVENTS; tagpu_recframe.h says
   what a frame is and what it shares): every event: RK_EV_CREATE..RK_EV_DEATH_END and
   RK_EV_NET_CREATE of a sectioned block.

   THE BYTES. Scheme 0, one column: for each record of its kinds in record order, a varint length
   and the body verbatim.
   THE STATE. The block's column, and the decoder's cursor in it: nothing crosses a block, and
   all of it is cleared at every reset.
   THE BOUNDS. A length is at most RECF_BODY_MAX and the column's bytes left; the column is read
   to its end. */
#include "tagpu_recframe.h"

#define EVENTS_SCHEME 0
#define EVENTS_COLS   1

typedef struct Events {
    RecBytes col;               /* the encoder's column, this block's */
    RecCol   rd;                /* the decoder's                      */
} Events;

static void* events_make(const RecCodecParams* p)
{
    (void)p;
    return calloc(1, sizeof(Events));
}

static void events_drop(void* f)
{
    Events* x = (Events*)f;
    if (!x) return;
    rb_free(&x->col);
    free(x);
}

static void events_restart(void* f) { rb_clear(&((Events*)f)->col); }

static int events_copy(void* dst, const void* src)
{
    (void)dst;
    (void)src;
    return 1;
}

static void events_enc_iter(void* f, const RecFrameAt* a)
{
    if (!a->it) rb_clear(&((Events*)f)->col);
}

static void events_enc_rec(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                           unsigned len)
{
    Events* x = (Events*)f;
    (void)a;
    (void)kind;
    rb_varint(&x->col, len);
    rb_put(&x->col, b, len);
}

static void events_enc_iter_end(void* f, const RecFrameAt* a)
{
    (void)f;
    (void)a;
}

static int events_enc_write(void* f, RecBytes* sec)
{
    return recf_write(sec, EVENTS_SCHEME, &((Events*)f)->col, EVENTS_COLS);
}

static int events_dec_open(void* f, const unsigned char* sec, size_t n, unsigned iterations,
                           char* why, unsigned cap)
{
    (void)iterations;
    return recf_open(sec, n, EVENTS_SCHEME, EVENTS_COLS, &((Events*)f)->rd, "events", why, cap);
}

static int events_dec_iter(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int events_dec_rec(void* f, const RecFrameAt* a, unsigned kind, unsigned char* body,
                          unsigned* len, char* why, unsigned cap)
{
    Events* x = (Events*)f;
    uint32_t n;
    (void)a;
    if (!recf_varint(&x->rd, &n) || n > RECF_BODY_MAX || !recf_bytes(&x->rd, body, n))
        RECF_REFUSE("an event (kind %u) cut short or past %u bytes in the events frame",
                    kind, RECF_BODY_MAX);
    *len = n;
    return 1;
}

static int events_dec_iter_end(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int events_dec_close(void* f, char* why, unsigned cap)
{
    return recf_close(&((Events*)f)->rd, EVENTS_COLS, "events", why, cap);
}

const RecFrameOps recframe_events = {
    "events", RCS_EVENTS,
    events_make, events_drop, events_restart, events_copy,
    events_enc_iter, events_enc_rec, events_enc_iter_end, events_enc_write,
    events_dec_open, events_dec_iter, events_dec_rec, events_dec_iter_end, events_dec_close
};
