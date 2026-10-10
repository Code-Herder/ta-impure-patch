#ifndef TAGPU_RECFRAME_H
#define TAGPU_RECFRAME_H
/* tagpu_recframe.h -- the stream codec's record frames (tagpu_reccodec.c drives them): the cells
   frame, the orders frame, the events frame and the state frame, one file each
   (tagpu_recframe_cells.c, _orders.c, _events.c, _state.c), each the maker of the bodies of its
   section's record kinds (tagpu_recfmt.h, RCS_CELLS, RCS_ORDERS, RCS_EVENTS, RCS_STATE), and what
   they share: the buffers and varints, the columns, where a record stands, and the operations
   the codec calls. Portable C99 like the codec: a frame file names no engine address in code and
   has no writable static; each frame's state belongs to one RecCodec, owned by one thread.

   THE ORDERING every frame rests on (tagpu_reccodec.c, THREE INVARIANTS). A frame's state is a
   function of the records since the last restart and of the codec's unit copy alone. The restart
   (RBF_FRAME_RESET) clears it -- every frame starts empty, in the writer's encoder, its check
   decoder and every reader alike -- and the unit copy at a record is the same in every walk: the
   unit frame's deltas of the iteration, then its RK_UNIT and RK_UNIT_GONE records up to that
   record. So a decline or a refusal needs no rollback: the codec turns invalid, and the next block
   restarts.

   A FRAME NEVER DECLINES MID-BLOCK. Its encoder takes every record of its kinds; one it cannot
   carry, or a failed allocation, latches inside the frame, and its enc_write then returns 0,
   which declines the block after its iterations, as the unit frame's RECC_FRAME_CAP does. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_reccodec.h"

#define RECF_REFUSE(...) do { if (why && cap) snprintf(why, cap, __VA_ARGS__); return 0; } while (0)

/* ---- buffers and varints -------------------------------------------------------------------- */

/* a RecBytes grown to hold `more` bytes past n: 0, latching oom, when it cannot */
static __inline int rb_grow(RecBytes* b, size_t more)
{
    size_t need, cap;
    unsigned char* p;
    if (b->oom) return 0;
    if (more > (size_t)-1 - b->n) {
        b->oom = 1;
        return 0;
    }
    need = b->n + more;
    if (need <= b->cap) return 1;
    cap = b->cap ? b->cap : 4096;
    while (cap < need) cap = cap > (size_t)-1 / 2 ? need : cap * 2;
    p = (unsigned char*)realloc(b->p, cap);
    if (!p) {
        b->oom = 1;
        return 0;
    }
    b->p = p;
    b->cap = cap;
    return 1;
}

static __inline void rb_put(RecBytes* b, const void* p, size_t n)
{
    if (n && rb_grow(b, n)) {
        memcpy(b->p + b->n, p, n);
        b->n += n;
    }
}

static __inline void rb_u8(RecBytes* b, unsigned v)
{
    unsigned char c = (unsigned char)v;
    rb_put(b, &c, 1);
}

static __inline void rb_u32(RecBytes* b, uint32_t v) { rb_put(b, &v, 4); }

static __inline void rb_varint(RecBytes* b, uint32_t v)
{
    unsigned char t[5];
    unsigned k = 0;
    do {
        t[k] = (unsigned char)(v & 0x7F);
        v >>= 7;
        if (v) t[k] |= 0x80;
        k++;
    } while (v);
    rb_put(b, t, k);
}

static __inline void rb_free(RecBytes* b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* empty, its latched failure cleared and its memory kept */
static __inline void rb_clear(RecBytes* b)
{
    b->n = 0;
    b->oom = 0;
}

/* LEB128 of a value below 2^32 from p[*at, end): at most 5 bytes, the fifth at most 0x0F;
   non-minimal forms read. 1, or 0 when it runs past `end` or past those bounds. */
static __inline int varint_get(const unsigned char* p, size_t end, size_t* at, uint32_t* v)
{
    uint32_t x = 0;
    unsigned i;
    for (i = 0; i < 5; i++) {
        unsigned b;
        if (*at >= end) return 0;
        b = p[(*at)++];
        if (i == 4 && b > 0x0F) return 0;
        x |= (uint32_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            *v = x;
            return 1;
        }
    }
    return 0;
}

static __inline unsigned get16(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static __inline uint32_t get32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static __inline void put32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* a 32-bit residual's zigzag: r < 2^31 -> 2r, else 2 (2^32 - r) - 1, and back */
static __inline uint32_t zz32(uint32_t r)
{
    return r < 0x80000000u ? 2u * r : (uint32_t)(2 * (UINT64_C(0x100000000) - r) - 1);
}
static __inline uint32_t unzz32(uint32_t z) { return (0u - (z & 1)) ^ (z >> 1); }

/* ---- THE COLUMNS ------------------------------------------------------------------------------

   A record frame's section is
       u8      scheme             the frame's coding: a reader takes its own scheme alone, so a
                                  file of another build's coding is refused, never misread
       varint  ncols              1..RECF_COLS_MAX: a reader takes its own count alone
       varint  len[ncols]         each column's bytes
       bytes   the columns, in order, which fill the section exactly
   and a reader consumes every column to its last byte. The encoder writes each column into a
   RecBytes of its own; the decoder reads each through a RecCol. */

#define RECF_COLS_MAX 32
#define RECF_BODY_MAX 0xFFFF   /* a record body a frame makes: RecHdr's u16 length            */

/* Each frame file states its column count through this, at file scope: a count past
   RECF_COLS_MAX would index recf_open's len[] and the Python fcols past their ends, so it fails
   the build instead (recf_open and recf_write also refuse one at run time). */
#define RECF_COLS_CHECK(name, n) \
    typedef char recf_cols_ok_##name[((n) >= 1 && (n) <= RECF_COLS_MAX) ? 1 : -1]

/* n bytes onto a record body the decoder is building, at *len: 1, or 0 when the body would pass
   RECF_BODY_MAX, the size of the buffer dec_rec writes into. Every byte a frame's dec_rec puts
   in its body goes through this, so the bound holds before the write, not after it. */
static __inline int recf_body_put(unsigned char* body, unsigned* len, const void* src, size_t n)
{
    if (n > (size_t)RECF_BODY_MAX - *len) return 0;
    memcpy(body + *len, src, n);
    *len += (unsigned)n;
    return 1;
}

typedef struct RecCol { const unsigned char* p; size_t at, end; } RecCol;

/* The section from `ncols` columns into `sec` (emptied first): 1, or 0 when a column or the
   section failed an allocation or passes RECC_FRAME_CAP -- the frame's enc_write then declines. */
static __inline int recf_write(RecBytes* sec, unsigned scheme, const RecBytes* col, unsigned ncols)
{
    unsigned i;
    rb_clear(sec);
    if (ncols < 1 || ncols > RECF_COLS_MAX) return 0;
    for (i = 0; i < ncols; i++)
        if (col[i].oom || col[i].n > RECC_FRAME_CAP) return 0;
    rb_u8(sec, scheme);
    rb_varint(sec, ncols);
    for (i = 0; i < ncols; i++) rb_varint(sec, (uint32_t)col[i].n);
    for (i = 0; i < ncols; i++) rb_put(sec, col[i].p, col[i].n);
    return !sec->oom && sec->n <= RECC_FRAME_CAP;
}

/* The section p[0, n) as `ncols` columns of `scheme`: 1, or 0 with `why` naming the frame. */
static __inline int recf_open(const unsigned char* p, size_t n, unsigned scheme, unsigned ncols,
                              RecCol* col, const char* name, char* why, unsigned cap)
{
    size_t at = 1, len[RECF_COLS_MAX];
    uint32_t v;
    unsigned i;
    if (ncols < 1 || ncols > RECF_COLS_MAX)
        RECF_REFUSE("the %s frame reads %u columns, past 1..%u", name, ncols, RECF_COLS_MAX);
    if (n < 1) RECF_REFUSE("the %s frame has no scheme byte", name);
    if (p[0] != scheme) RECF_REFUSE("the %s frame is scheme %u, this build reads %u", name, p[0], scheme);
    if (!varint_get(p, n, &at, &v) || v != ncols)
        RECF_REFUSE("the %s frame's column count is not its %u", name, ncols);
    for (i = 0; i < ncols; i++) {
        if (!varint_get(p, n, &at, &v)) RECF_REFUSE("the %s frame's column table cut short", name);
        len[i] = v;
    }
    for (i = 0; i < ncols; i++) {
        if (len[i] > n - at) RECF_REFUSE("the %s frame's column %u runs past its section", name, i);
        col[i].p = p;
        col[i].at = at;
        at += len[i];
        col[i].end = at;
    }
    if (at != n) RECF_REFUSE("%lu bytes of the %s frame follow its columns", (unsigned long)(n - at), name);
    return 1;
}

/* every column read to its end: 1, or 0 with `why` */
static __inline int recf_close(const RecCol* col, unsigned ncols, const char* name, char* why,
                               unsigned cap)
{
    unsigned i;
    for (i = 0; i < ncols; i++)
        if (col[i].at != col[i].end)
            RECF_REFUSE("%lu bytes left in the %s frame's column %u", (unsigned long)(col[i].end - col[i].at), name, i);
    return 1;
}

/* one value from a column: 1, or 0 when it runs past the column's end */
static __inline int recf_varint(RecCol* c, uint32_t* v) { return varint_get(c->p, c->end, &c->at, v); }
static __inline int recf_u8(RecCol* c, unsigned* v)
{
    if (c->at >= c->end) return 0;
    *v = c->p[c->at++];
    return 1;
}
static __inline int recf_bytes(RecCol* c, void* dst, size_t n)
{
    if (n > c->end - c->at) return 0;
    memcpy(dst, c->p + c->at, n);
    c->at += n;
    return 1;
}

/* ---- the operations ------------------------------------------------------------------------- */

/* Where a frame record stands, the same in the encoder's walk and the decoder's. */
typedef struct RecFrameAt {
    unsigned it;                 /* the iteration's index in the block, 0 its first           */
    uint32_t gt;                 /* its RK_TICK's GameTime                                    */
    int events;                  /* an RCS_EVENTS record came earlier in this iteration: the
                                    reader's rule (tagpu_recreader.c `apply`), under which an
                                    RK_CELLS after one is the feature tick's, not an image     */
    unsigned nslots;
    const uint32_t* unit;        /* the codec's unit copy, nslots * REC_UNIT_DWORDS: the
                                    iteration's deltas, then its RK_UNIT and RK_UNIT_GONE
                                    records up to this one                                     */
    const unsigned char* live;   /* nslots                                                     */
} RecFrameAt;

/* A frame: its section, and what the codec calls. `f` is the state `make` returned. */
typedef struct RecFrameOps {
    const char* name;            /* for a refusal: "cells", "orders", "events", "state"       */
    unsigned section;            /* RCS_CELLS, RCS_ORDERS, RCS_EVENTS or RCS_STATE              */
    /* NULL: no memory. The params' sizes are the codec's for its life (reccodec_new). */
    void* (*make)(const RecCodecParams* p);
    void  (*drop)(void* f);
    /* every piece of state cleared, its latches with it: reccodec_restart */
    void  (*restart)(void* f);
    /* dst takes src's state, both made with the same params (tests/codectest.c's fuzz): 0, no
       memory */
    int   (*copy)(void* dst, const void* src);
    /* THE ENCODER, per iteration of a block: enc_iter (at `it` 0 the block's columns start
       empty), enc_rec for each record of its kinds in record order, enc_iter_end; then, after the
       block's last, enc_write: its section into `sec`, or 0 when it latched */
    void  (*enc_iter)(void* f, const RecFrameAt* a);
    void  (*enc_rec)(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                     unsigned len);
    void  (*enc_iter_end)(void* f, const RecFrameAt* a);
    int   (*enc_write)(void* f, RecBytes* sec);
    /* THE DECODER: dec_open takes the block's section and its iterations before the first; per
       iteration dec_iter, dec_rec for each record of its kinds -- its body into `body`, which
       holds RECF_BODY_MAX bytes, through recf_body_put alone, and its length -- and
       dec_iter_end; dec_close after the last. The caller's own test of the length after
       dec_rec names a frame that broke this; it is not the bound.
       Each returns 1, or 0 with `why`, and the block is refused. */
    int   (*dec_open)(void* f, const unsigned char* sec, size_t n, unsigned iterations, char* why,
                      unsigned cap);
    int   (*dec_iter)(void* f, const RecFrameAt* a, char* why, unsigned cap);
    int   (*dec_rec)(void* f, const RecFrameAt* a, unsigned kind, unsigned char* body,
                     unsigned* len, char* why, unsigned cap);
    int   (*dec_iter_end)(void* f, const RecFrameAt* a, char* why, unsigned cap);
    int   (*dec_close)(void* f, char* why, unsigned cap);
} RecFrameOps;

extern const RecFrameOps recframe_cells, recframe_orders, recframe_events, recframe_state;

#endif
