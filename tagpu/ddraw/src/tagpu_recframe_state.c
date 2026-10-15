/* tagpu_recframe_state.c -- the state frame (tagpu_recfmt.h, RCS_STATE; tagpu_recframe.h says what
   a frame is and what it shares): every RK_GLOBALS, RK_GLOBALS_DELTA, RK_PLAYER, RK_PLAYER_DELTA,
   RK_WRECK and RK_FEATURES of a sectioned block, each against what the frame last saw of it.

   THE BYTES. Scheme 1, nineteen columns (enum below). Per record, in record order:
   - RK_GLOBALS: in gfull, varint n (1..RECC_GLOBALS_MAX), then the n dwords raw.
   - RK_GLOBALS_DELTA: in gm, u8 0 for the last delta's mask, else its byte count (1..32), whose
     bytes follow in gmask. Then for each changed dword, ascending, a method byte in gmeth:
       0  varint zz((v - prev) - the dword's last delta), in gval;
       1  varint k (1..RECC_PM_CAP) in gsteps: v is k steps of f from prev, where
          f(s) = (s mod (2^31 - 1)) * 16807 mod (2^31 - 1), and 0 becomes 2^31 - 1;
       2  v raw, in graw.
     Methods 0 and 1 need a known dword, and at most RECC_PM_EACH dwords of an iteration take 1.
   - RK_PLAYER: the player, u8 in pl, and its image raw in pimg.
   - RK_PLAYER_DELTA: the player in pl; in pm, u8 1 for the player's last mask, else 0 and the
     REC_PLAYER_BITS mask bytes in pmask; then each changed dword, ascending: when known,
     varint zz((v - prev) - its last delta) in pval, else v raw in praw.
   - RK_WRECK: in wi, varint zz(index - (the iteration's previous index + 1)), the first against
     0. A known record: in wm, 3 bytes, bit j set when its u16 j changed, and those u16s raw in
     wv; an unknown one: its REC_WRECK_BYTES raw in wraw.
   - RK_FEATURES: in fm, u8, bit j set when field j misses its prediction, then those fields'
     varint zz(v - prediction) in fv. The three heads are predicted as last time; the cursor as
     `last ? last - 1 : W * H - 1`.
   The residuals are 32-bit, zigzagged (zz32). An RK_FEATURES that equals the prediction, after
   an explicit one since the reset, is no record of this frame: the kinds section's marker
   0xFFFF stands for it (tagpu_reccodec.c, THE KINDS), and the codec moves the fields on through
   recframe_state_predicted and recframe_state_take, as an explicit one moves them.

   THE STATE, cleared at every reset: the globals (RECC_GLOBALS_MAX dwords) with known bits, each
   one's last delta and the last delta's mask; players 0..REC_PLAYERS - 1, their images with
   known bits, last deltas and last masks; the wreck pool's records (RT_FEATURES' count) with
   known bits; the RK_FEATURES fields, zero after a reset. A dword's last delta is v - prev when
   it was known, else 0, whatever its method. A full image makes its dwords known and keeps the
   last deltas. Per iteration: the method-1 count and the next wreck index. The encoder alone
   keeps a flag per global that method 1 carried its last change: only there does it search up
   to RECC_PM_CAP steps, elsewhere RECC_PM_NEAR (MEASURED 2026-10-09 by the L3 research on the
   battle: the seed moves a median of 16 steps an iteration, at most 12,622).
   f is a size rule, not a claim about the engine: a value it does not reach within the search
   goes by method 0 or raw, so the decode never rests on f matching the RNG. It is the engine's
   RNG step, which is why it reaches the seed: DISASSEMBLED 2026-10-10, 0x4B6C30 returns 0 for
   n < 2 without stepping (0x4B6C35..0x4B6C3D); otherwise it steps the seed [0x51FC88] x 16807
   mod 2^31 - 1, adds 0x7FFFFFFF to a result <= 0 (0x4B6C7C..0x4B6C80) and stores the seed at
   0x4B6C87. The cursor's prediction is the feature tick's: it decrements main+0x14257
   (0x4240A9..0x4240AA) and below 0 sets it to W * H - 1 (0x4240BE..0x4240CC), DISASSEMBLED
   2026-10-10.

   THE BOUNDS. Every value read is tested before use and a refusal names it: a full image's count
   (1..RECC_GLOBALS_MAX), a mask's byte count (1..32, and a same-mask flag only after a mask), a
   method (0..2, 0 and 1 on a known dword only), a step count (1..RECC_PM_CAP) and the method-1
   count (RECC_PM_EACH an iteration: at most 2 x 16,384 steps), a player (< REC_PLAYERS), a
   player mask's bits (< REC_PLAYER_DWORDS) and flag (0 or 1), a wreck index (< RT_FEATURES'
   count), a feature mask (< 16); every column read stops at its end and is read to it. Every
   byte of a body goes through recf_body_put, which refuses before RECF_BODY_MAX; the longest is
   a globals delta of 32 mask bytes and 256 dwords, 1,056 bytes. The encoder latches on a record
   it cannot carry back byte for byte -- a length that is not its kind's, a pad that is not zero,
   a player, wreck or mask bit past its bound -- and its enc_write then declines the block.

   MEMORY: about 15 KB, and REC_WRECK_BYTES + 1 bytes per wreck record (393 KB at 8,192). */
#include "tagpu_recframe.h"

#define STATE_SCHEME     1
#define RECC_GLOBALS_MAX 256      /* the globals image's dwords at most: a 32-byte mask      */
#define RECC_PM_CAP      16384    /* method 1's steps at most                                */
#define RECC_PM_NEAR     64       /* the encoder's search where method 1 did not win last    */
#define RECC_PM_EACH     2        /* method-1 dwords an iteration at most                    */
#define PM_M             0x7FFFFFFFu
#define WRECK_HALVES     (REC_WRECK_BYTES / 2)
#define PLAYER_HEAD      REC_PLAYER_DELTA_HEAD

_Static_assert(REC_GLOBALS_DWORDS >= 1 && REC_GLOBALS_BITS <= RECC_GLOBALS_MAX / 8,
               "the globals' mask fits the state frame's 32 bytes");
_Static_assert(WRECK_HALVES == 24, "a wreck record's mask is 3 bytes");
_Static_assert(REC_PLAYER_DELTA_HEAD == 2 + REC_PLAYER_BITS + 1, "one pad byte after a player mask");

enum {
    C_GFULL, C_GM, C_GMASK, C_GMETH, C_GVAL, C_GSTEPS, C_GRAW,
    C_PL, C_PM, C_PMASK, C_PIMG, C_PVAL, C_PRAW,
    C_WI, C_WM, C_WV, C_WRAW,
    C_FM, C_FV,
    STATE_COLS
};
RECF_COLS_CHECK(state, STATE_COLS);

/* what the frame knows: everything a reset clears but the wreck records */
typedef struct Known {
    uint32_t g[RECC_GLOBALS_MAX], gdel[RECC_GLOBALS_MAX];
    unsigned char gknown[RECC_GLOBALS_MAX];
    unsigned char gwon[RECC_GLOBALS_MAX];          /* the encoder's: method 1 carried it last */
    unsigned char gmask[RECC_GLOBALS_MAX / 8];
    unsigned gmaskn;                                /* 0: no delta yet                         */
    uint32_t pv[REC_PLAYERS][REC_PLAYER_DWORDS], pdel[REC_PLAYERS][REC_PLAYER_DWORDS];
    unsigned char pknown[REC_PLAYERS][REC_PLAYER_DWORDS];
    unsigned char pmask[REC_PLAYERS][REC_PLAYER_BITS];
    uint32_t feat[4];
    unsigned npm;                                   /* this iteration's method-1 dwords        */
    uint32_t wnext;                                 /* this iteration's previous wreck + 1     */
    int bad;                                        /* the encoder latched                     */
} Known;

typedef struct State {
    unsigned nwrecks;
    uint32_t cells;                 /* W * H: at most RECC_GRID_MAX (reccodec_params)          */
    Known k;
    unsigned char* w;               /* nwrecks records of REC_WRECK_BYTES                     */
    unsigned char* wknown;          /* nwrecks                                                */
    RecBytes col[STATE_COLS];       /* the encoder's columns, this block's                    */
    RecCol rd[STATE_COLS];          /* the decoder's                                          */
} State;

static int bit_at(const unsigned char* m, unsigned i) { return m[i >> 3] >> (i & 7) & 1; }

static uint32_t pm_step(uint32_t s)
{
    uint32_t t = (uint32_t)((uint64_t)(s % PM_M) * 16807u % PM_M);
    return t ? t : PM_M;
}

/* the steps of f from `from` to `to`, 1..cap, or 0 when it is not reached */
static unsigned pm_steps(uint32_t from, uint32_t to, unsigned cap)
{
    unsigned k;
    for (k = 1; k <= cap; k++) {
        from = pm_step(from);
        if (from == to) return k;
    }
    return 0;
}

/* reccodec_new makes a frame only for params within sizes_ok: 1 <= nwrecks, 1 <= gw * gh */
static void* state_make(const RecCodecParams* p)
{
    State* x = (State*)calloc(1, sizeof(State));
    if (!x) return NULL;
    x->nwrecks = p->nwrecks;
    x->cells = p->gw * p->gh;
    x->w = (unsigned char*)calloc(p->nwrecks, REC_WRECK_BYTES);
    x->wknown = (unsigned char*)calloc(p->nwrecks, 1);
    if (!x->w || !x->wknown) {
        free(x->w);
        free(x->wknown);
        free(x);
        return NULL;
    }
    return x;
}

static void state_drop(void* f)
{
    State* x = (State*)f;
    unsigned i;
    if (!x) return;
    for (i = 0; i < STATE_COLS; i++) rb_free(&x->col[i]);
    free(x->w);
    free(x->wknown);
    free(x);
}

/* the wreck records' bytes need no clearing: a record is read only under its known bit */
static void state_restart(void* f, const RecFrameWorld* w)
{
    State* x = (State*)f;
    unsigned i;
    (void)w;
    memset(&x->k, 0, sizeof x->k);
    memset(x->wknown, 0, x->nwrecks);
    for (i = 0; i < STATE_COLS; i++) rb_clear(&x->col[i]);
}

#ifdef RECF_TEST_COPY
static int state_copy(void* dst, const void* src)
{
    State* d = (State*)dst;
    const State* s = (const State*)src;
    d->k = s->k;
    memcpy(d->w, s->w, (size_t)s->nwrecks * REC_WRECK_BYTES);
    memcpy(d->wknown, s->wknown, s->nwrecks);
    return 1;
}
#endif

/* ---- the encoder ------------------------------------------------------------------------------ */

static void state_enc_iter(void* f, const RecFrameAt* a)
{
    State* x = (State*)f;
    unsigned i;
    if (!a->it)
        for (i = 0; i < STATE_COLS; i++) rb_clear(&x->col[i]);
    x->k.npm = 0;
    x->k.wnext = 0;
}

static void enc_global(State* x, unsigned d, uint32_t v)
{
    Known* k = &x->k;
    if (!k->gknown[d]) {
        rb_u8(&x->col[C_GMETH], 2);
        rb_u32(&x->col[C_GRAW], v);
        k->gdel[d] = 0;
    } else {
        uint32_t dl = v - k->g[d], z = zz32(dl - k->gdel[d]);
        unsigned steps = 0;
        if (z && k->npm < RECC_PM_EACH)
            steps = pm_steps(k->g[d], v, k->gwon[d] ? RECC_PM_CAP : RECC_PM_NEAR);
        if (steps) {
            rb_u8(&x->col[C_GMETH], 1);
            rb_varint(&x->col[C_GSTEPS], steps);
            k->npm++;
        } else if (z < (1u << 28)) {
            rb_u8(&x->col[C_GMETH], 0);
            rb_varint(&x->col[C_GVAL], z);
        } else {
            /* a varint of z would take 5 bytes */
            rb_u8(&x->col[C_GMETH], 2);
            rb_u32(&x->col[C_GRAW], v);
        }
        k->gwon[d] = steps != 0;
        k->gdel[d] = dl;
    }
    k->g[d] = v;
    k->gknown[d] = 1;
}

static void enc_globals(State* x, const unsigned char* b, unsigned len)
{
    unsigned n = len / 4, d;
    if (len % 4 || n < 1 || n > RECC_GLOBALS_MAX) {
        x->k.bad = 1;
        return;
    }
    rb_varint(&x->col[C_GFULL], n);
    rb_put(&x->col[C_GFULL], b, len);
    for (d = 0; d < RECC_GLOBALS_MAX; d++) {
        x->k.gknown[d] = d < n;
        x->k.g[d] = d < n ? get32(b + 4 * d) : 0;
    }
}

static void enc_globals_delta(State* x, const unsigned char* b, unsigned len)
{
    Known* k = &x->k;
    unsigned d, n = 0, j;
    if (len < REC_GLOBALS_DELTA_HEAD) {
        k->bad = 1;
        return;
    }
    for (d = REC_GLOBALS_BITS; d < REC_GLOBALS_DELTA_HEAD; d++)
        if (b[d]) k->bad = 1;
    for (d = 0; d < 8 * REC_GLOBALS_BITS; d++) n += bit_at(b, d);
    if (k->bad || len != REC_GLOBALS_DELTA_HEAD + 4 * n) {
        k->bad = 1;
        return;
    }
    if (k->gmaskn == REC_GLOBALS_BITS && !memcmp(k->gmask, b, REC_GLOBALS_BITS)) {
        rb_u8(&x->col[C_GM], 0);
    } else {
        rb_u8(&x->col[C_GM], REC_GLOBALS_BITS);
        rb_put(&x->col[C_GMASK], b, REC_GLOBALS_BITS);
        memcpy(k->gmask, b, REC_GLOBALS_BITS);
        k->gmaskn = REC_GLOBALS_BITS;
    }
    for (d = 0, j = 0; d < 8 * REC_GLOBALS_BITS; d++)
        if (bit_at(b, d)) enc_global(x, d, get32(b + REC_GLOBALS_DELTA_HEAD + 4 * j++));
}

static void enc_player(State* x, const unsigned char* b, unsigned len)
{
    unsigned p = b[0], i;
    if (len != 4 + REC_PLAYER_IMAGE || b[1] || p >= REC_PLAYERS || b[2] || b[3]) {
        x->k.bad = 1;
        return;
    }
    rb_u8(&x->col[C_PL], p);
    rb_put(&x->col[C_PIMG], b + 4, REC_PLAYER_IMAGE);
    for (i = 0; i < REC_PLAYER_DWORDS; i++) {
        x->k.pv[p][i] = get32(b + 4 + 4 * i);
        x->k.pknown[p][i] = 1;
    }
}

static void enc_player_delta(State* x, const unsigned char* b, unsigned len)
{
    Known* k = &x->k;
    unsigned p, i, j, n = 0;
    if (len < PLAYER_HEAD || b[1] || b[0] >= REC_PLAYERS || b[PLAYER_HEAD - 1]) {
        k->bad = 1;
        return;
    }
    p = b[0];
    for (i = 0; i < 8 * REC_PLAYER_BITS; i++)
        if (bit_at(b + 2, i)) {
            if (i >= REC_PLAYER_DWORDS) k->bad = 1;
            n++;
        }
    if (k->bad || len != PLAYER_HEAD + 4 * n) {
        k->bad = 1;
        return;
    }
    rb_u8(&x->col[C_PL], p);
    if (!memcmp(k->pmask[p], b + 2, REC_PLAYER_BITS)) {
        rb_u8(&x->col[C_PM], 1);
    } else {
        rb_u8(&x->col[C_PM], 0);
        rb_put(&x->col[C_PMASK], b + 2, REC_PLAYER_BITS);
        memcpy(k->pmask[p], b + 2, REC_PLAYER_BITS);
    }
    for (i = 0, j = 0; i < REC_PLAYER_DWORDS; i++)
        if (bit_at(b + 2, i)) {
            uint32_t v = get32(b + PLAYER_HEAD + 4 * j++);
            if (k->pknown[p][i]) {
                uint32_t dl = v - k->pv[p][i];
                rb_varint(&x->col[C_PVAL], zz32(dl - k->pdel[p][i]));
                k->pdel[p][i] = dl;
            } else {
                rb_u32(&x->col[C_PRAW], v);
                k->pdel[p][i] = 0;
            }
            k->pv[p][i] = v;
            k->pknown[p][i] = 1;
        }
}

static void enc_wreck(State* x, const unsigned char* b, unsigned len)
{
    unsigned i, j;
    unsigned char* old;
    if (len != 4 + REC_WRECK_BYTES || b[2] || b[3] || get16(b) >= x->nwrecks) {
        x->k.bad = 1;
        return;
    }
    i = get16(b);
    rb_varint(&x->col[C_WI], zz32((uint32_t)i - x->k.wnext));
    x->k.wnext = i + 1;
    old = x->w + (size_t)i * REC_WRECK_BYTES;
    if (x->wknown[i]) {
        uint32_t m = 0;
        unsigned char mb[3];
        for (j = 0; j < WRECK_HALVES; j++)
            if (memcmp(old + 2 * j, b + 4 + 2 * j, 2)) m |= 1u << j;
        mb[0] = (unsigned char)m;
        mb[1] = (unsigned char)(m >> 8);
        mb[2] = (unsigned char)(m >> 16);
        rb_put(&x->col[C_WM], mb, 3);
        for (j = 0; j < WRECK_HALVES; j++)
            if (m >> j & 1) rb_put(&x->col[C_WV], b + 4 + 2 * j, 2);
    } else {
        rb_put(&x->col[C_WRAW], b + 4, REC_WRECK_BYTES);
    }
    memcpy(old, b + 4, REC_WRECK_BYTES);
    x->wknown[i] = 1;
}

/* the RK_FEATURES fields' predictions from the last: the heads as they were, the cursor one
   cell on, from 0 to the grid's last */
static void feat_predict(const State* x, uint32_t pred[4])
{
    pred[0] = x->k.feat[0];
    pred[1] = x->k.feat[1];
    pred[2] = x->k.feat[2];
    pred[3] = x->k.feat[3] ? x->k.feat[3] - 1 : x->cells - 1;
}

void recframe_state_predicted(const void* f, unsigned char out[16])
{
    uint32_t pred[4];
    unsigned j;
    feat_predict((const State*)f, pred);
    for (j = 0; j < 4; j++) put32(out + 4 * j, pred[j]);
}

void recframe_state_take(void* f, const unsigned char b[16])
{
    State* x = (State*)f;
    unsigned j;
    for (j = 0; j < 4; j++) x->k.feat[j] = get32(b + 4 * j);
}

static void enc_features(State* x, const unsigned char* b, unsigned len)
{
    uint32_t pred[4], v;
    unsigned j, m = 0;
    if (len != 16) {
        x->k.bad = 1;
        return;
    }
    feat_predict(x, pred);
    for (j = 0; j < 4; j++)
        if (get32(b + 4 * j) != pred[j]) m |= 1u << j;
    rb_u8(&x->col[C_FM], m);
    for (j = 0; j < 4; j++) {
        v = get32(b + 4 * j);
        if (m >> j & 1) rb_varint(&x->col[C_FV], zz32(v - pred[j]));
        x->k.feat[j] = v;
    }
}

static void state_enc_rec(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                          unsigned len)
{
    State* x = (State*)f;
    (void)a;
    if (x->k.bad) return;
    switch (kind) {
    case RK_GLOBALS:       enc_globals(x, b, len); break;
    case RK_GLOBALS_DELTA: enc_globals_delta(x, b, len); break;
    case RK_PLAYER:        enc_player(x, b, len); break;
    case RK_PLAYER_DELTA:  enc_player_delta(x, b, len); break;
    case RK_WRECK:         enc_wreck(x, b, len); break;
    case RK_FEATURES:      enc_features(x, b, len); break;
    default:               x->k.bad = 1; break;
    }
}

static void state_enc_iter_end(void* f, const RecFrameAt* a)
{
    (void)f;
    (void)a;
}

static int state_enc_write(void* f, RecBytes* sec)
{
    State* x = (State*)f;
    if (x->k.bad) return 0;
    return recf_write(sec, STATE_SCHEME, x->col, STATE_COLS);
}

/* ---- the decoder ------------------------------------------------------------------------------ */

static int state_dec_open(void* f, const unsigned char* sec, size_t n, unsigned iterations,
                          char* why, unsigned cap)
{
    (void)iterations;
    return recf_open(sec, n, STATE_SCHEME, STATE_COLS, ((State*)f)->rd, "state", why, cap);
}

static int state_dec_iter(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    State* x = (State*)f;
    (void)a;
    (void)why;
    (void)cap;
    x->k.npm = 0;
    x->k.wnext = 0;
    return 1;
}

static int col_u32(RecCol* c, uint32_t* v)
{
    unsigned char t[4];
    if (!recf_bytes(c, t, 4)) return 0;
    *v = get32(t);
    return 1;
}

static int dec_global(State* x, unsigned d, uint32_t* v, char* why, unsigned cap)
{
    Known* k = &x->k;
    unsigned meth;
    uint32_t z;
    if (!recf_u8(&x->rd[C_GMETH], &meth)) RECF_REFUSE("the state frame's global methods cut short");
    if (meth > 2) RECF_REFUSE("global %u's method %u", d, meth);
    if (meth < 2 && !k->gknown[d]) RECF_REFUSE("global %u is unknown under method %u", d, meth);
    if (meth == 0) {
        if (!recf_varint(&x->rd[C_GVAL], &z)) RECF_REFUSE("the state frame's global residuals cut short");
        *v = k->g[d] + k->gdel[d] + unzz32(z);
    } else if (meth == 1) {
        unsigned i;
        uint32_t s = k->g[d];
        if (!recf_varint(&x->rd[C_GSTEPS], &z)) RECF_REFUSE("the state frame's global steps cut short");
        if (z < 1 || z > RECC_PM_CAP) RECF_REFUSE("global %u takes %u steps, not 1..%u", d, z, RECC_PM_CAP);
        if (k->npm >= RECC_PM_EACH) RECF_REFUSE("more than %u globals an iteration by steps", RECC_PM_EACH);
        k->npm++;
        for (i = 0; i < z; i++) s = pm_step(s);
        *v = s;
    } else if (!col_u32(&x->rd[C_GRAW], v)) {
        RECF_REFUSE("the state frame's raw globals cut short");
    }
    k->gdel[d] = k->gknown[d] ? *v - k->g[d] : 0;
    k->g[d] = *v;
    k->gknown[d] = 1;
    return 1;
}

/* n bytes onto the body (recf_body_put), or the record is refused */
#define BODY_PUT(src, n)                                                                           \
    do {                                                                                           \
        if (!recf_body_put(body, len, (src), (n)))                                                 \
            RECF_REFUSE("a state record's body past %u bytes", RECF_BODY_MAX);                     \
    } while (0)

static int body_u32(unsigned char* body, unsigned* len, uint32_t v)
{
    unsigned char t[4];
    put32(t, v);
    return recf_body_put(body, len, t, 4);
}

static int dec_globals(State* x, unsigned char* body, unsigned* len, char* why, unsigned cap)
{
    unsigned char img[4 * RECC_GLOBALS_MAX];
    uint32_t n;
    unsigned d;
    if (!recf_varint(&x->rd[C_GFULL], &n)) RECF_REFUSE("the state frame's global images cut short");
    if (n < 1 || n > RECC_GLOBALS_MAX) RECF_REFUSE("a globals image of %u dwords, not 1..%u", n, RECC_GLOBALS_MAX);
    if (!recf_bytes(&x->rd[C_GFULL], img, 4 * n)) RECF_REFUSE("a globals image cut short");
    BODY_PUT(img, 4 * n);
    for (d = 0; d < RECC_GLOBALS_MAX; d++) {
        x->k.gknown[d] = d < n;
        x->k.g[d] = d < n ? get32(img + 4 * d) : 0;
    }
    return 1;
}

static int dec_globals_delta(State* x, unsigned char* body, unsigned* len, char* why, unsigned cap)
{
    static const unsigned char pad[3];
    Known* k = &x->k;
    unsigned m, d;
    if (!recf_u8(&x->rd[C_GM], &m)) RECF_REFUSE("the state frame's global masks cut short");
    if (m > RECC_GLOBALS_MAX / 8) RECF_REFUSE("a globals mask of %u bytes", m);
    if (!m) {
        if (!k->gmaskn) RECF_REFUSE("a globals delta repeats a mask before the first");
    } else {
        if (!recf_bytes(&x->rd[C_GMASK], k->gmask, m)) RECF_REFUSE("a globals mask cut short");
        k->gmaskn = m;
    }
    BODY_PUT(k->gmask, k->gmaskn);
    BODY_PUT(pad, ((k->gmaskn + 3) & ~3u) - k->gmaskn);
    for (d = 0; d < 8 * k->gmaskn; d++)
        if (bit_at(k->gmask, d)) {
            uint32_t v;
            if (!dec_global(x, d, &v, why, cap)) return 0;
            if (!body_u32(body, len, v)) RECF_REFUSE("a state record's body past %u bytes", RECF_BODY_MAX);
        }
    return 1;
}

static int dec_player(State* x, unsigned char* body, unsigned* len, char* why, unsigned cap)
{
    unsigned char head[4] = { 0, 0, 0, 0 }, img[REC_PLAYER_IMAGE];
    unsigned p, i;
    if (!recf_u8(&x->rd[C_PL], &p)) RECF_REFUSE("the state frame's players cut short");
    if (p >= REC_PLAYERS) RECF_REFUSE("player %u in the state frame", p);
    if (!recf_bytes(&x->rd[C_PIMG], img, REC_PLAYER_IMAGE)) RECF_REFUSE("a player image cut short");
    head[0] = (unsigned char)p;
    BODY_PUT(head, 4);
    BODY_PUT(img, REC_PLAYER_IMAGE);
    for (i = 0; i < REC_PLAYER_DWORDS; i++) {
        x->k.pv[p][i] = get32(img + 4 * i);
        x->k.pknown[p][i] = 1;
    }
    return 1;
}

static int dec_player_delta(State* x, unsigned char* body, unsigned* len, char* why, unsigned cap)
{
    Known* k = &x->k;
    unsigned char head[PLAYER_HEAD];
    unsigned p, same, i;
    if (!recf_u8(&x->rd[C_PL], &p)) RECF_REFUSE("the state frame's players cut short");
    if (p >= REC_PLAYERS) RECF_REFUSE("player %u in the state frame", p);
    if (!recf_u8(&x->rd[C_PM], &same)) RECF_REFUSE("the state frame's player mask flags cut short");
    if (same > 1) RECF_REFUSE("a player mask flag %u", same);
    if (!same) {
        if (!recf_bytes(&x->rd[C_PMASK], k->pmask[p], REC_PLAYER_BITS)) RECF_REFUSE("a player mask cut short");
        for (i = REC_PLAYER_DWORDS; i < 8 * REC_PLAYER_BITS; i++)
            if (bit_at(k->pmask[p], i)) RECF_REFUSE("player %u's mask names dword %u", p, i);
    }
    head[0] = (unsigned char)p;
    head[1] = 0;
    memcpy(head + 2, k->pmask[p], REC_PLAYER_BITS);
    head[PLAYER_HEAD - 1] = 0;
    BODY_PUT(head, PLAYER_HEAD);
    for (i = 0; i < REC_PLAYER_DWORDS; i++)
        if (bit_at(k->pmask[p], i)) {
            uint32_t v, z;
            if (k->pknown[p][i]) {
                if (!recf_varint(&x->rd[C_PVAL], &z)) RECF_REFUSE("the state frame's player residuals cut short");
                v = k->pv[p][i] + k->pdel[p][i] + unzz32(z);
                k->pdel[p][i] = v - k->pv[p][i];
            } else {
                if (!col_u32(&x->rd[C_PRAW], &v)) RECF_REFUSE("the state frame's raw player dwords cut short");
                k->pdel[p][i] = 0;
            }
            k->pv[p][i] = v;
            k->pknown[p][i] = 1;
            if (!body_u32(body, len, v)) RECF_REFUSE("a state record's body past %u bytes", RECF_BODY_MAX);
        }
    return 1;
}

static int dec_wreck(State* x, unsigned char* body, unsigned* len, char* why, unsigned cap)
{
    unsigned char head[4];
    uint32_t z, i;
    unsigned char* rec;
    if (!recf_varint(&x->rd[C_WI], &z)) RECF_REFUSE("the state frame's wreck indexes cut short");
    i = x->k.wnext + unzz32(z);
    if (i >= x->nwrecks) RECF_REFUSE("wreck record %u past the pool's %u", i, x->nwrecks);
    x->k.wnext = i + 1;
    rec = x->w + (size_t)i * REC_WRECK_BYTES;
    if (x->wknown[i]) {
        unsigned char mb[3];
        unsigned j;
        uint32_t m;
        if (!recf_bytes(&x->rd[C_WM], mb, 3)) RECF_REFUSE("the state frame's wreck masks cut short");
        m = (uint32_t)mb[0] | (uint32_t)mb[1] << 8 | (uint32_t)mb[2] << 16;
        for (j = 0; j < WRECK_HALVES; j++)
            if (m >> j & 1 && !recf_bytes(&x->rd[C_WV], rec + 2 * j, 2))
                RECF_REFUSE("the state frame's wreck values cut short");
    } else {
        if (!recf_bytes(&x->rd[C_WRAW], rec, REC_WRECK_BYTES)) RECF_REFUSE("the state frame's raw wrecks cut short");
        x->wknown[i] = 1;
    }
    head[0] = (unsigned char)i;
    head[1] = (unsigned char)(i >> 8);
    head[2] = head[3] = 0;
    BODY_PUT(head, 4);
    BODY_PUT(rec, REC_WRECK_BYTES);
    return 1;
}

static int dec_features(State* x, unsigned char* body, unsigned* len, char* why, unsigned cap)
{
    uint32_t pred[4], z;
    unsigned m, j;
    if (!recf_u8(&x->rd[C_FM], &m)) RECF_REFUSE("the state frame's feature masks cut short");
    if (m > 15) RECF_REFUSE("a feature mask %u", m);
    feat_predict(x, pred);
    for (j = 0; j < 4; j++) {
        uint32_t v = pred[j];
        if (m >> j & 1) {
            if (!recf_varint(&x->rd[C_FV], &z)) RECF_REFUSE("the state frame's feature residuals cut short");
            v += unzz32(z);
        }
        x->k.feat[j] = v;
        if (!body_u32(body, len, v)) RECF_REFUSE("a state record's body past %u bytes", RECF_BODY_MAX);
    }
    return 1;
}

static int state_dec_rec(void* f, const RecFrameAt* a, unsigned kind, unsigned char* body,
                         unsigned* len, char* why, unsigned cap)
{
    State* x = (State*)f;
    (void)a;
    *len = 0;
    switch (kind) {
    case RK_GLOBALS:       return dec_globals(x, body, len, why, cap);
    case RK_GLOBALS_DELTA: return dec_globals_delta(x, body, len, why, cap);
    case RK_PLAYER:        return dec_player(x, body, len, why, cap);
    case RK_PLAYER_DELTA:  return dec_player_delta(x, body, len, why, cap);
    case RK_WRECK:         return dec_wreck(x, body, len, why, cap);
    case RK_FEATURES:      return dec_features(x, body, len, why, cap);
    default:               RECF_REFUSE("kind %u in the state frame", kind);
    }
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
    return recf_close(((State*)f)->rd, STATE_COLS, "state", why, cap);
}

const RecFrameOps recframe_state = {
    "state", RCS_STATE,
    state_make, state_drop, state_restart, NULL, NULL, NULL,
#ifdef RECF_TEST_COPY
    state_copy,
#endif
    state_enc_iter, state_enc_rec, state_enc_iter_end, state_enc_write,
    state_dec_open, state_dec_iter, state_dec_rec, state_dec_iter_end, state_dec_close
};
