/* tagpu_recframe_state.c -- the state frame (tagpu_recfmt.h, RCS_STATE; tagpu_recframe.h says what
   a frame is and what it shares): every RK_GLOBALS, RK_GLOBALS_DELTA, RK_PLAYER, RK_PLAYER_DELTA,
   RK_WRECK and RK_FEATURES of a sectioned block.

   THE BYTES. Scheme 0, one column: for each record of its kinds in record order, a varint length
   and the body verbatim.
   THE STATE. The block's column, and the decoder's cursor in it: nothing crosses a block, and
   all of it is cleared at every reset.
   THE BOUNDS. A length is at most RECF_BODY_MAX and the column's bytes left; the column is read
   to its end. */
#include "tagpu_recframe.h"

#define STATE_SCHEME 0
#define STATE_COLS   1

typedef struct State {
    RecBytes col;               /* the encoder's column, this block's */
    RecCol   rd;                /* the decoder's                      */
} State;

static void* state_make(const RecCodecParams* p)
{
    (void)p;
    return calloc(1, sizeof(State));
}

static void state_drop(void* f)
{
    State* x = (State*)f;
    if (!x) return;
    rb_free(&x->col);
    free(x);
}

static void state_restart(void* f) { rb_clear(&((State*)f)->col); }

static int state_copy(void* dst, const void* src)
{
    (void)dst;
    (void)src;
    return 1;
}

static void state_enc_iter(void* f, const RecFrameAt* a)
{
    if (!a->it) rb_clear(&((State*)f)->col);
}

static void state_enc_rec(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                          unsigned len)
{
    State* x = (State*)f;
    (void)a;
    (void)kind;
    rb_varint(&x->col, len);
    rb_put(&x->col, b, len);
}

static void state_enc_iter_end(void* f, const RecFrameAt* a)
{
    (void)f;
    (void)a;
}

static int state_enc_write(void* f, RecBytes* sec)
{
    return recf_write(sec, STATE_SCHEME, &((State*)f)->col, STATE_COLS);
}

static int state_dec_open(void* f, const unsigned char* sec, size_t n, unsigned iterations,
                          char* why, unsigned cap)
{
    (void)iterations;
    return recf_open(sec, n, STATE_SCHEME, STATE_COLS, &((State*)f)->rd, "state", why, cap);
}

static int state_dec_iter(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int state_dec_rec(void* f, const RecFrameAt* a, unsigned kind, unsigned char* body,
                         unsigned* len, char* why, unsigned cap)
{
    State* x = (State*)f;
    uint32_t n;
    (void)a;
    if (!recf_varint(&x->rd, &n) || n > RECF_BODY_MAX || !recf_bytes(&x->rd, body, n))
        RECF_REFUSE("a state record (kind %u) cut short or past %u bytes in the state frame",
                    kind, RECF_BODY_MAX);
    *len = n;
    return 1;
}

static int state_dec_iter_end(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int state_dec_close(void* f, char* why, unsigned cap)
{
    return recf_close(&((State*)f)->rd, STATE_COLS, "state", why, cap);
}

const RecFrameOps recframe_state = {
    "state", RCS_STATE,
    state_make, state_drop, state_restart, state_copy,
    state_enc_iter, state_enc_rec, state_enc_iter_end, state_enc_write,
    state_dec_open, state_dec_iter, state_dec_rec, state_dec_iter_end, state_dec_close
};
