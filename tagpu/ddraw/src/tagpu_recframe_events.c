/* tagpu_recframe_events.c -- the events frame (tagpu_recfmt.h, RCS_EVENTS; tagpu_recframe.h says
   what a frame is and what it shares): every event, RK_EV_CREATE..RK_EV_DEATH_END,
   RK_EV_NET_CREATE and RK_EV_SLICE, of a sectioned block, each field in a column of its own and coded against
   what the frame knows at that record: the last unit ID, the codec's unit copy, and the events
   since the reset.

   ENGINE FACTS the coding rests on, as sizes only -- a prediction that misses costs bytes, never
   a value:
   - A shot's start is the firing piece's point (tagpu_recfmt.h, RK_EV_SHOT), and 0x49D9C0 sends
     the shooter's +0x6A..+0x75 in its place (0x49DA9F..0x49DAC7); the receiver 0x49D270 reads the
     u16 shooter at +0x21 (0x49D329..0x49D34A) and the u16 target at +0x1F (0x49D388..0x49D3B7)
     (DISASSEMBLED, exe-reverse-engineering.md, what a shot's packet says of its start). MEASURED
     by the L3 research on format-9 recordings: the start lies within 63 world units of the
     shooter's +0x6A/+0x6E/+0x72 and the target point within 15 of the target's, 99.9 % of shots.
   - A death's image is the unit as the destructor 0x4866D0 found it (RK_EV_DEATH); MEASURED by
     the same research, it differs from the unit copy in 0 to 17 dwords.

   THE BYTES. Scheme 1, EV_COLS columns (C_* below), every record's fields in record order. An ID
   is coded as zz(ID - the last ID coded), and the last ID becomes it.
     RK_EV_SHOT        C_ID the shooter (+0x21, at most 0xFFFF); C_SH_TGT 0 for a target of
                       0xFFFF, else 1 + zz(target - shooter); C_SH_START the start's x, y, z and
                       C_SH_TARGET the target point's, each zz(point - its unit's +0x6A, +0x6E,
                       +0x72), against zero where the unit is no live slot; C_SH_RAW +0x00,
                       +0x19..+0x1E and +0x23
     RK_EV_START       C_ID; C_ST_IDX zz(index); C_ST_ARGC zz(argc); C_ST_RZ 1 when every
                       argument from min(argc, 4) on is zero, and those are not coded; C_ST_ARGS
                       the others, each zz(arg - the slot's last arguments for this script, else
                       zero)
     RK_EV_START_NAMED C_ID; C_NM_HEAD argc and now; C_ST_RZ as a start's; C_ST_ARGS zz(arg);
                       C_NM_NAME 1 + the name's index in the names, or 0 and C_LIT its length
                       and bytes
     RK_EV_REPLY       C_ID; C_RP_KIND the kind; C_RP_HEAD root and text given; C_RP_TEXT and
                       C_LIT the text as a name's, in the texts
     RK_EV_SPRAY       C_ID; C_SP_MODE 0: the slot's last spray again; 1: its reverse and its
                       tail from +0x14 on again, C_SP_POINT zz(x, y, z - its last); 2: C_SP_RAW
                       the 44 bytes after the ID
     RK_EV_DEATH       C_ID; C_DT_KILLER zz(killer - ID); C_DT_HEAD +8..+0xB; C_DT_MASK the
                       REC_UNIT_BITS bytes of the image's dwords that differ from the unit copy's
                       (zero where the unit is no live slot); C_DT_DWORDS those dwords
     RK_EV_DEATH_END   nothing: its ID is the innermost open death's
     RK_EV_FEATURE     C_FT_HEAD root and phase; C_FT_SAME 1 when the site is the root's last,
                       else 0 and C_FT_SITE the site; C_FT_ARGS the root's arguments
                       (rec_root_len)
     RK_EV_FEATURE_END C_FE_ROOT the root; C_FE_RESULT the result
     RK_EV_CREATE      C_ID; C_CREATE +4..+0x18
     RK_EV_NET_CREATE  C_ID; C_CREATE +4..+0x1D
     RK_EV_DRAW        C_DRAW +0 and +4..+0xB
     RK_EV_BUILT       C_ID; C_RAW the built unit's ID
     RK_EV_STATE       C_ID; C_RAW +4
     RK_EV_INTERCEPT   C_RAW +0..+0xD
     RK_EV_SLICE       C_ID
   The bytes a kind's comment in tagpu_recfmt.h names 0 are not carried: the decoder writes zero
   there. The encoder latches, and its enc_write declines the block, on an event whose pad is not
   zero or whose length fails its kind's rule, a death past REC_DEATHS_MAX open ones, and a
   death's end that is not the innermost open death's.

   THE STATE, cleared at every reset (events_restart): the last unit ID; the names and the texts,
   each the first 256 literals since the reset; per slot (ID - 1 < nslots) its last spray and four
   (script index, arguments) entries, the oldest replaced by a script none of them holds; per
   feature root its last site; the open deaths, innermost last; the encoder's latch. MEMORY: 128
   bytes a slot (640 KB at 5,001 slots) and 40 KB of dictionaries, besides the columns.

   THE BOUNDS. Every value read is bounded before it is used: a shot's IDs at 0xFFFF and its
   target below it, a slot by nslots, a dictionary index by the entries held, a literal by
   REC_NAME_MAX or REC_TEXT_MAX, a root by FR_ROOTS, the open deaths by REC_DEATHS_MAX, a death's
   mask by REC_UNIT_DWORDS, a spray's mode by 2 and its modes 0 and 1 by a slot that sprayed, a
   flag by 1; every column is read to its end. The decoder builds an event in a buffer of the
   longest event's EV_BODY_MAX bytes, at offsets each kind's fixed layout bounds, and puts it in
   the body through recf_body_put. */
#include "tagpu_recframe.h"

#define EVENTS_SCHEME 1

enum {
    C_ID, C_SH_TGT, C_SH_START, C_SH_TARGET, C_SH_RAW,
    C_ST_IDX, C_ST_ARGC, C_ST_RZ, C_ST_ARGS,
    C_NM_HEAD, C_NM_NAME, C_LIT,
    C_RP_KIND, C_RP_HEAD, C_RP_TEXT,
    C_SP_MODE, C_SP_POINT, C_SP_RAW,
    C_DT_KILLER, C_DT_HEAD, C_DT_MASK, C_DT_DWORDS,
    C_FT_HEAD, C_FT_SAME, C_FT_SITE, C_FT_ARGS,
    C_FE_ROOT, C_FE_RESULT,
    C_CREATE, C_DRAW, C_RAW,
    EV_COLS
};
RECF_COLS_CHECK(events, EV_COLS);

#define EV_DICT    256         /* the literals a dictionary keeps                              */
#define EV_STARTS  4           /* the (script, arguments) entries a slot keeps                 */
#define EV_SPRAY   44          /* an RK_EV_SPRAY after its ID                                  */
#define SHOT_LEN   36
#define SH_NONE    0xFFFFu     /* a shot's unit past the array                                 */
#define EV_BODY_MAX (12 + REC_UNIT_IMAGE)   /* the longest event, a death: the decoder builds each
                                               in a buffer of this, then puts it in the body     */
_Static_assert(EV_BODY_MAX >= 24 + REC_NAME_MAX && EV_BODY_MAX >= 12 + REC_TEXT_MAX &&
               EV_BODY_MAX >= 8 + REC_ROOT_ARGS_MAX && EV_BODY_MAX >= sizeof(RecNetCreate),
               "every event fits the decoder's buffer");

typedef struct EvSlot {
    uint32_t start[EV_STARTS][5];       /* script index, arguments x4                          */
    unsigned char spray[EV_SPRAY];
    unsigned char sprayed, nstart, next;
} EvSlot;

typedef struct EvDict {
    unsigned n;
    unsigned char len[EV_DICT];
    unsigned char* text;                /* EV_DICT entries of `max` bytes                       */
    unsigned max;
} EvDict;

typedef struct Events {
    unsigned nslots;
    uint32_t last;
    EvDict names, texts;
    EvSlot* slot;
    uint32_t site[FR_ROOTS];
    unsigned char sited[FR_ROOTS];
    uint32_t death[REC_DEATHS_MAX];
    unsigned ndeaths;
    int latched;                        /* the encoder met an event it cannot carry            */
    RecBytes col[EV_COLS];              /* the encoder's columns, this block's                 */
    RecCol rd[EV_COLS];                 /* the decoder's                                       */
} Events;

/* ---- the state ------------------------------------------------------------------------------ */

static void events_drop(void* f)
{
    Events* x = (Events*)f;
    unsigned i;
    if (!x) return;
    for (i = 0; i < EV_COLS; i++) rb_free(&x->col[i]);
    free(x->names.text);
    free(x->texts.text);
    free(x->slot);
    free(x);
}

static void* events_make(const RecCodecParams* p)
{
    Events* x = (Events*)calloc(1, sizeof(Events));
    if (!x) return NULL;
    x->nslots = p->nslots;
    x->names.max = REC_NAME_MAX;
    x->texts.max = REC_TEXT_MAX;
    x->names.text = (unsigned char*)calloc(EV_DICT, REC_NAME_MAX);
    x->texts.text = (unsigned char*)calloc(EV_DICT, REC_TEXT_MAX);
    x->slot = (EvSlot*)calloc(p->nslots ? p->nslots : 1, sizeof(EvSlot));
    if (!x->names.text || !x->texts.text || !x->slot) {
        events_drop(x);
        return NULL;
    }
    return x;
}

static void events_restart(void* f, const RecFrameWorld* w)
{
    Events* x = (Events*)f;
    unsigned i;
    (void)w;
    x->last = 0;
    x->names.n = 0;
    x->texts.n = 0;
    memset(x->slot, 0, (size_t)x->nslots * sizeof(EvSlot));
    memset(x->site, 0, sizeof x->site);
    memset(x->sited, 0, sizeof x->sited);
    x->ndeaths = 0;
    x->latched = 0;
    for (i = 0; i < EV_COLS; i++) rb_clear(&x->col[i]);
}

#ifdef RECF_TEST_COPY
static void dict_copy(EvDict* d, const EvDict* s)
{
    d->n = s->n;
    memcpy(d->len, s->len, sizeof d->len);
    memcpy(d->text, s->text, (size_t)EV_DICT * s->max);
}

static int events_copy(void* dst, const void* src)
{
    Events* d = (Events*)dst;
    const Events* s = (const Events*)src;
    d->last = s->last;
    dict_copy(&d->names, &s->names);
    dict_copy(&d->texts, &s->texts);
    memcpy(d->slot, s->slot, (size_t)s->nslots * sizeof(EvSlot));
    memcpy(d->site, s->site, sizeof d->site);
    memcpy(d->sited, s->sited, sizeof d->sited);
    memcpy(d->death, s->death, sizeof d->death);
    d->ndeaths = s->ndeaths;
    d->latched = s->latched;
    return 1;
}
#endif

/* ---- what both sides derive ------------------------------------------------------------------- */

/* the slot's state of a unit ID, or NULL past the slots */
static EvSlot* slot_of(Events* x, uint32_t id)
{
    return id >= 1 && id - 1 < x->nslots ? &x->slot[id - 1] : NULL;
}

/* the unit copy's image of a unit ID, or NULL where the ID is no live slot */
static const unsigned char* unit_of(const RecFrameAt* a, uint32_t id)
{
    if (id < 1 || id - 1 >= a->nslots || !a->live[id - 1]) return NULL;
    return (const unsigned char*)(a->unit + (size_t)(id - 1) * REC_UNIT_DWORDS);
}

/* a unit's +0x6A, +0x6E, +0x72, zero for a shot's ID 0xFFFF and where it is no live slot */
static void pos_of(const RecFrameAt* a, uint32_t id, uint32_t p[3])
{
    const unsigned char* u = id == SH_NONE ? NULL : unit_of(a, id);
    unsigned k;
    for (k = 0; k < 3; k++) p[k] = u ? get32(u + 0x6A + 4 * k) : 0;
}

/* the entry of a slot's starts that holds this script, or -1 */
static int start_find(const EvSlot* s, uint32_t idx)
{
    unsigned i;
    for (i = 0; i < s->nstart; i++)
        if (s->start[i][0] == idx) return (int)i;
    return -1;
}

/* the slot's entry for this script takes the arguments; a script none holds replaces the oldest */
static void start_keep(EvSlot* s, uint32_t idx, const uint32_t* args)
{
    int i = start_find(s, idx);
    if (i < 0) {
        i = s->next;
        s->next = (unsigned char)((s->next + 1) % EV_STARTS);
        if (s->nstart < EV_STARTS) s->nstart++;
        s->start[i][0] = idx;
    }
    memcpy(&s->start[i][1], args, 16);
}

/* a literal's index in the dictionary, or -1 */
static int dict_find(const EvDict* d, const unsigned char* p, unsigned n)
{
    unsigned i;
    for (i = 0; i < d->n; i++)
        if (d->len[i] == n && !memcmp(d->text + (size_t)i * d->max, p, n)) return (int)i;
    return -1;
}

/* a literal joins the dictionary while it has room */
static void dict_add(EvDict* d, const unsigned char* p, unsigned n)
{
    if (d->n >= EV_DICT) return;
    d->len[d->n] = (unsigned char)n;
    memcpy(d->text + (size_t)d->n * d->max, p, n);
    d->n++;
}

/* an RK_EV_FEATURE's argument bytes for a root below FR_ROOTS: rec_root_len's own */
static unsigned root_args(unsigned root)
{
    unsigned k;
    for (k = 0; k <= REC_ROOT_ARGS_MAX; k++)
        if (rec_root_len(root, 8 + k)) return k;
    return 0;
}

/* ---- THE ENCODER -------------------------------------------------------------------------------- */

static int zero(const unsigned char* p, unsigned n)
{
    while (n--)
        if (*p++) return 0;
    return 1;
}

/* the event's length and pads as its kind's comment in tagpu_recfmt.h gives them */
static int well_formed(unsigned kind, const unsigned char* b, unsigned len)
{
    switch (kind) {
    case RK_EV_CREATE:      return len == 28 && zero(b + 25, 3);
    case RK_EV_NET_CREATE:  return len == sizeof(RecNetCreate) && zero(b + 30, 2);
    case RK_EV_DEATH:       return len == 12 + REC_UNIT_IMAGE;
    case RK_EV_SHOT:        return len == SHOT_LEN;
    case RK_EV_INTERCEPT:   return len == 16 && zero(b + 14, 2);
    case RK_EV_START:       return len == 28;
    case RK_EV_START_NAMED: return len >= 24 && len <= 24 + REC_NAME_MAX && zero(b + 6, 2);
    case RK_EV_BUILT:       return len == 8;
    case RK_EV_STATE:       return len == 8 && zero(b + 5, 3);
    case RK_EV_REPLY:       return len >= 12 && len <= 12 + REC_TEXT_MAX && zero(b + 10, 2);
    case RK_EV_SPRAY:       return len == 48;
    case RK_EV_DRAW:        return len == 12 && zero(b + 1, 3);
    case RK_EV_FEATURE:     return len >= 8 && rec_root_len(b[0], len) && zero(b + 2, 2);
    case RK_EV_FEATURE_END: return len == 8 && zero(b + 1, 3);
    case RK_EV_DEATH_END:   return len == 4;
    case RK_EV_SLICE:       return len == 4;
    default:                return 0;
    }
}

static void put_id(Events* x, uint32_t id)
{
    rb_varint(&x->col[C_ID], zz32(id - x->last));
    x->last = id;
}

static void put_zz(Events* x, unsigned c, uint32_t v, uint32_t pred)
{
    rb_varint(&x->col[c], zz32(v - pred));
}

/* the arguments of a start, against pred[4]: the zero flag, then the ones it does not cover */
static void put_args(Events* x, const unsigned char* b, unsigned argc, const uint32_t* pred)
{
    unsigned na = argc < 4 ? argc : 4, rz = 1, i;
    for (i = na; i < 4; i++)
        if (get32(b + 4 * i)) rz = 0;
    rb_u8(&x->col[C_ST_RZ], rz);
    for (i = 0; i < (rz ? na : 4); i++) put_zz(x, C_ST_ARGS, get32(b + 4 * i), pred ? pred[i] : 0);
}

static void put_text(Events* x, EvDict* d, unsigned c, const unsigned char* p, unsigned n)
{
    int i = dict_find(d, p, n);
    if (i >= 0) {
        rb_varint(&x->col[c], (uint32_t)i + 1);
        return;
    }
    rb_varint(&x->col[c], 0);
    rb_varint(&x->col[C_LIT], n);
    rb_put(&x->col[C_LIT], p, n);
    dict_add(d, p, n);
}

static void enc_shot(Events* x, const RecFrameAt* a, const unsigned char* b)
{
    uint32_t sh = get16(b + 0x21), tg = get16(b + 0x1F), p[3];
    unsigned k;
    put_id(x, sh);
    rb_varint(&x->col[C_SH_TGT], tg == SH_NONE ? 0 : 1 + zz32(tg - sh));
    pos_of(a, sh, p);
    for (k = 0; k < 3; k++) put_zz(x, C_SH_START, get32(b + 1 + 4 * k), p[k]);
    pos_of(a, tg, p);
    for (k = 0; k < 3; k++) put_zz(x, C_SH_TARGET, get32(b + 0xD + 4 * k), p[k]);
    rb_u8(&x->col[C_SH_RAW], b[0]);
    rb_put(&x->col[C_SH_RAW], b + 0x19, 6);
    rb_u8(&x->col[C_SH_RAW], b[0x23]);
}

static void enc_start(Events* x, const unsigned char* b)
{
    uint32_t id = get32(b), idx = get32(b + 4), argc = get32(b + 8), args[4];
    EvSlot* s = slot_of(x, id);
    int e = s ? start_find(s, idx) : -1;
    unsigned k;
    put_id(x, id);
    put_zz(x, C_ST_IDX, idx, 0);
    put_zz(x, C_ST_ARGC, argc, 0);
    put_args(x, b + 12, argc, e >= 0 ? &s->start[e][1] : NULL);
    for (k = 0; k < 4; k++) args[k] = get32(b + 12 + 4 * k);
    if (s) start_keep(s, idx, args);
}

static void enc_spray(Events* x, const unsigned char* b)
{
    uint32_t id = get32(b);
    EvSlot* s = slot_of(x, id);
    const unsigned char* t = b + 4;
    unsigned k;
    put_id(x, id);
    if (s && s->sprayed && !memcmp(s->spray, t, EV_SPRAY)) {
        rb_u8(&x->col[C_SP_MODE], 0);
    } else if (s && s->sprayed && !memcmp(s->spray, t, 4) && !memcmp(s->spray + 16, t + 16, EV_SPRAY - 16)) {
        rb_u8(&x->col[C_SP_MODE], 1);
        for (k = 0; k < 3; k++) put_zz(x, C_SP_POINT, get32(t + 4 + 4 * k), get32(s->spray + 4 + 4 * k));
    } else {
        rb_u8(&x->col[C_SP_MODE], 2);
        rb_put(&x->col[C_SP_RAW], t, EV_SPRAY);
    }
    if (s) {
        memcpy(s->spray, t, EV_SPRAY);
        s->sprayed = 1;
    }
}

static void enc_death(Events* x, const RecFrameAt* a, const unsigned char* b)
{
    uint32_t id = get32(b);
    const unsigned char* u = unit_of(a, id);
    const unsigned char* img = b + 12;
    unsigned char mask[REC_UNIT_BITS];
    unsigned i;
    if (x->ndeaths >= REC_DEATHS_MAX) {
        x->latched = 1;
        return;
    }
    x->death[x->ndeaths++] = id;
    put_id(x, id);
    put_zz(x, C_DT_KILLER, get32(b + 4), id);
    rb_put(&x->col[C_DT_HEAD], b + 8, 4);
    memset(mask, 0, sizeof mask);
    for (i = 0; i < REC_UNIT_DWORDS; i++)
        if (get32(img + 4 * i) != (u ? get32(u + 4 * i) : 0)) mask[i >> 3] |= (unsigned char)(1u << (i & 7));
    rb_put(&x->col[C_DT_MASK], mask, sizeof mask);
    for (i = 0; i < REC_UNIT_DWORDS; i++)
        if (mask[i >> 3] >> (i & 7) & 1) rb_put(&x->col[C_DT_DWORDS], img + 4 * i, 4);
}

static void enc_feature(Events* x, const unsigned char* b, unsigned len)
{
    unsigned root = b[0];
    uint32_t site = get32(b + 4);
    rb_put(&x->col[C_FT_HEAD], b, 2);
    if (x->sited[root] && x->site[root] == site) {
        rb_u8(&x->col[C_FT_SAME], 1);
    } else {
        rb_u8(&x->col[C_FT_SAME], 0);
        rb_put(&x->col[C_FT_SITE], b + 4, 4);
    }
    x->site[root] = site;
    x->sited[root] = 1;
    rb_put(&x->col[C_FT_ARGS], b + 8, len - 8);
}

static void events_enc_iter(void* f, const RecFrameAt* a)
{
    Events* x = (Events*)f;
    unsigned i;
    if (!a->it)
        for (i = 0; i < EV_COLS; i++) rb_clear(&x->col[i]);
}

static void events_enc_rec(void* f, const RecFrameAt* a, unsigned kind, const unsigned char* b,
                           unsigned len)
{
    Events* x = (Events*)f;
    if (x->latched) return;
    if (!well_formed(kind, b, len)) {
        x->latched = 1;
        return;
    }
    switch (kind) {
    case RK_EV_SHOT:
        enc_shot(x, a, b);
        break;
    case RK_EV_START:
        enc_start(x, b);
        break;
    case RK_EV_START_NAMED:
        put_id(x, get32(b));
        rb_put(&x->col[C_NM_HEAD], b + 4, 2);
        put_args(x, b + 8, b[4], NULL);
        put_text(x, &x->names, C_NM_NAME, b + 24, len - 24);
        break;
    case RK_EV_REPLY:
        put_id(x, get32(b));
        rb_varint(&x->col[C_RP_KIND], get32(b + 4));
        rb_put(&x->col[C_RP_HEAD], b + 8, 2);
        put_text(x, &x->texts, C_RP_TEXT, b + 12, len - 12);
        break;
    case RK_EV_SPRAY:
        enc_spray(x, b);
        break;
    case RK_EV_DEATH:
        enc_death(x, a, b);
        break;
    case RK_EV_DEATH_END:
        if (!x->ndeaths || x->death[x->ndeaths - 1] != get32(b)) x->latched = 1;
        else x->ndeaths--;
        break;
    case RK_EV_FEATURE:
        enc_feature(x, b, len);
        break;
    case RK_EV_FEATURE_END:
        rb_u8(&x->col[C_FE_ROOT], b[0]);
        rb_varint(&x->col[C_FE_RESULT], get32(b + 4));
        break;
    case RK_EV_CREATE:
        put_id(x, get32(b));
        rb_put(&x->col[C_CREATE], b + 4, 21);
        break;
    case RK_EV_NET_CREATE:
        put_id(x, get32(b));
        rb_put(&x->col[C_CREATE], b + 4, 26);
        break;
    case RK_EV_DRAW:
        rb_u8(&x->col[C_DRAW], b[0]);
        rb_put(&x->col[C_DRAW], b + 4, 8);
        break;
    case RK_EV_BUILT:
        put_id(x, get32(b));
        rb_put(&x->col[C_RAW], b + 4, 4);
        break;
    case RK_EV_STATE:
        put_id(x, get32(b));
        rb_u8(&x->col[C_RAW], b[4]);
        break;
    case RK_EV_INTERCEPT:
        rb_put(&x->col[C_RAW], b, 14);
        break;
    case RK_EV_SLICE:
        put_id(x, get32(b));
        break;
    }
}

static void events_enc_iter_end(void* f, const RecFrameAt* a)
{
    (void)f;
    (void)a;
}

static int events_enc_write(void* f, RecBytes* sec)
{
    Events* x = (Events*)f;
    return !x->latched && recf_write(sec, EVENTS_SCHEME, x->col, EV_COLS);
}

/* ---- THE DECODER -------------------------------------------------------------------------------- */

#define CUT(c) RECF_REFUSE("an event (kind %u) cut short in the events frame's column %u", kind, (unsigned)(c))

static int events_dec_open(void* f, const unsigned char* sec, size_t n, unsigned iterations,
                           char* why, unsigned cap)
{
    (void)iterations;
    return recf_open(sec, n, EVENTS_SCHEME, EV_COLS, ((Events*)f)->rd, "events", why, cap);
}

static int events_dec_iter(void* f, const RecFrameAt* a, char* why, unsigned cap)
{
    (void)f;
    (void)a;
    (void)why;
    (void)cap;
    return 1;
}

static int get_id(Events* x, uint32_t* id)
{
    uint32_t v;
    if (!recf_varint(&x->rd[C_ID], &v)) return 0;
    *id = x->last + unzz32(v);
    x->last = *id;
    return 1;
}

static int get_zz(Events* x, unsigned c, uint32_t pred, unsigned char* out)
{
    uint32_t v;
    if (!recf_varint(&x->rd[c], &v)) return 0;
    put32(out, pred + unzz32(v));
    return 1;
}

/* a flag of column c: 0 or 1, else -1 (cut short: -2) */
static int get_flag(Events* x, unsigned c)
{
    unsigned v;
    if (!recf_u8(&x->rd[c], &v)) return -2;
    return v <= 1 ? (int)v : -1;
}

/* a start's arguments into out[16] against pred[4]: 1, 0 cut short, -1 a bad flag */
static int get_args(Events* x, unsigned char* out, uint32_t argc, const uint32_t* pred)
{
    unsigned na = argc < 4 ? argc : 4, i;
    int rz = get_flag(x, C_ST_RZ);
    if (rz < 0) return rz == -2 ? 0 : -1;
    memset(out, 0, 16);
    for (i = 0; i < (rz ? na : 4u); i++)
        if (!get_zz(x, C_ST_ARGS, pred ? pred[i] : 0, out + 4 * i)) return 0;
    return 1;
}

/* a name or a text into out from the dictionary or a literal of at most d->max bytes: its
   length, or -1 cut short, -2 an index past the entries, -3 a literal past the bound */
static int get_text(Events* x, EvDict* d, unsigned c, unsigned char* out)
{
    uint32_t v, n;
    if (!recf_varint(&x->rd[c], &v)) return -1;
    if (v) {
        if (v > d->n) return -2;
        memcpy(out, d->text + (size_t)(v - 1) * d->max, d->len[v - 1]);
        return d->len[v - 1];
    }
    if (!recf_varint(&x->rd[C_LIT], &n)) return -1;
    if (n > d->max) return -3;
    if (!recf_bytes(&x->rd[C_LIT], out, n)) return -1;
    dict_add(d, out, n);
    return (int)n;
}

static int events_dec_rec(void* f, const RecFrameAt* a, unsigned kind, unsigned char* out,
                          unsigned* len, char* why, unsigned cap)
{
    Events* x = (Events*)f;
    unsigned char body[EV_BODY_MAX];
    uint32_t id, v, p[3];
    unsigned k, blen;
    int r;
    switch (kind) {
    case RK_EV_SHOT: {
        uint32_t tg;
        if (!get_id(x, &id)) CUT(C_ID);
        if (id > 0xFFFF) RECF_REFUSE("a shot's shooter ID %lu is past 0xFFFF", (unsigned long)id);
        if (!recf_varint(&x->rd[C_SH_TGT], &v)) CUT(C_SH_TGT);
        tg = v ? id + unzz32(v - 1) : SH_NONE;
        if (v && tg >= SH_NONE) RECF_REFUSE("a shot's target ID %lu is past 0xFFFE", (unsigned long)tg);
        memset(body, 0, SHOT_LEN);
        pos_of(a, id, p);
        for (k = 0; k < 3; k++)
            if (!get_zz(x, C_SH_START, p[k], body + 1 + 4 * k)) CUT(C_SH_START);
        pos_of(a, tg, p);
        for (k = 0; k < 3; k++)
            if (!get_zz(x, C_SH_TARGET, p[k], body + 0xD + 4 * k)) CUT(C_SH_TARGET);
        if (!recf_bytes(&x->rd[C_SH_RAW], body, 1) || !recf_bytes(&x->rd[C_SH_RAW], body + 0x19, 6) ||
            !recf_bytes(&x->rd[C_SH_RAW], body + 0x23, 1))
            CUT(C_SH_RAW);
        body[0x1F] = (unsigned char)tg;
        body[0x20] = (unsigned char)(tg >> 8);
        body[0x21] = (unsigned char)id;
        body[0x22] = (unsigned char)(id >> 8);
        blen = SHOT_LEN;
        break;
    }
    case RK_EV_START: {
        uint32_t idx, argc, args[4];
        EvSlot* s;
        int e;
        if (!get_id(x, &id)) CUT(C_ID);
        if (!recf_varint(&x->rd[C_ST_IDX], &v)) CUT(C_ST_IDX);
        idx = unzz32(v);
        if (!recf_varint(&x->rd[C_ST_ARGC], &v)) CUT(C_ST_ARGC);
        argc = unzz32(v);
        s = slot_of(x, id);
        e = s ? start_find(s, idx) : -1;
        r = get_args(x, body + 12, argc, e >= 0 ? &s->start[e][1] : NULL);
        if (!r) CUT(C_ST_ARGS);
        if (r < 0) RECF_REFUSE("a start's argument flag is not 0 or 1");
        put32(body, id);
        put32(body + 4, idx);
        put32(body + 8, argc);
        for (k = 0; k < 4; k++) args[k] = get32(body + 12 + 4 * k);
        if (s) start_keep(s, idx, args);
        blen = 28;
        break;
    }
    case RK_EV_START_NAMED:
        if (!get_id(x, &id)) CUT(C_ID);
        memset(body, 0, 24);
        put32(body, id);
        if (!recf_bytes(&x->rd[C_NM_HEAD], body + 4, 2)) CUT(C_NM_HEAD);
        r = get_args(x, body + 8, body[4], NULL);
        if (!r) CUT(C_ST_ARGS);
        if (r < 0) RECF_REFUSE("a named start's argument flag is not 0 or 1");
        r = get_text(x, &x->names, C_NM_NAME, body + 24);
        if (r == -1) CUT(C_NM_NAME);
        if (r < 0) RECF_REFUSE("a named start's name is past the names or past %u bytes", REC_NAME_MAX);
        blen = 24 + (unsigned)r;
        break;
    case RK_EV_REPLY:
        if (!get_id(x, &id)) CUT(C_ID);
        memset(body, 0, 12);
        put32(body, id);
        if (!recf_varint(&x->rd[C_RP_KIND], &v)) CUT(C_RP_KIND);
        put32(body + 4, v);
        if (!recf_bytes(&x->rd[C_RP_HEAD], body + 8, 2)) CUT(C_RP_HEAD);
        r = get_text(x, &x->texts, C_RP_TEXT, body + 12);
        if (r == -1) CUT(C_RP_TEXT);
        if (r < 0) RECF_REFUSE("a reply's text is past the texts or past %u bytes", REC_TEXT_MAX);
        blen = 12 + (unsigned)r;
        break;
    case RK_EV_SPRAY: {
        EvSlot* s;
        unsigned mode;
        unsigned char* t = body + 4;
        if (!get_id(x, &id)) CUT(C_ID);
        put32(body, id);
        s = slot_of(x, id);
        if (!recf_u8(&x->rd[C_SP_MODE], &mode)) CUT(C_SP_MODE);
        if (mode > 2 || (mode < 2 && !(s && s->sprayed)))
            RECF_REFUSE("spray mode %u for unit ID %lu", mode, (unsigned long)id);
        if (mode == 2) {
            if (!recf_bytes(&x->rd[C_SP_RAW], t, EV_SPRAY)) CUT(C_SP_RAW);
        } else {
            memcpy(t, s->spray, EV_SPRAY);
            if (mode == 1)
                for (k = 0; k < 3; k++)
                    if (!get_zz(x, C_SP_POINT, get32(s->spray + 4 + 4 * k), t + 4 + 4 * k)) CUT(C_SP_POINT);
        }
        if (s) {
            memcpy(s->spray, t, EV_SPRAY);
            s->sprayed = 1;
        }
        blen = 48;
        break;
    }
    case RK_EV_DEATH: {
        const unsigned char* u;
        unsigned char mask[REC_UNIT_BITS];
        unsigned char* img = body + 12;
        if (x->ndeaths >= REC_DEATHS_MAX) RECF_REFUSE("deaths nested past %u", REC_DEATHS_MAX);
        if (!get_id(x, &id)) CUT(C_ID);
        x->death[x->ndeaths++] = id;
        put32(body, id);
        if (!get_zz(x, C_DT_KILLER, id, body + 4)) CUT(C_DT_KILLER);
        if (!recf_bytes(&x->rd[C_DT_HEAD], body + 8, 4)) CUT(C_DT_HEAD);
        if (!recf_bytes(&x->rd[C_DT_MASK], mask, sizeof mask)) CUT(C_DT_MASK);
        for (k = REC_UNIT_DWORDS; k < 8 * REC_UNIT_BITS; k++)
            if (mask[k >> 3] >> (k & 7) & 1) RECF_REFUSE("a death's mask names dword %u of %u", k, REC_UNIT_DWORDS);
        u = unit_of(a, id);
        if (u) memcpy(img, u, REC_UNIT_IMAGE);
        else memset(img, 0, REC_UNIT_IMAGE);
        for (k = 0; k < REC_UNIT_DWORDS; k++)
            if (mask[k >> 3] >> (k & 7) & 1 && !recf_bytes(&x->rd[C_DT_DWORDS], img + 4 * k, 4)) CUT(C_DT_DWORDS);
        blen = 12 + REC_UNIT_IMAGE;
        break;
    }
    case RK_EV_DEATH_END:
        if (!x->ndeaths) RECF_REFUSE("an RK_EV_DEATH_END with no death open");
        put32(body, x->death[--x->ndeaths]);
        blen = 4;
        break;
    case RK_EV_FEATURE: {
        unsigned root, n;
        if (!recf_bytes(&x->rd[C_FT_HEAD], body, 2)) CUT(C_FT_HEAD);
        root = body[0];
        if (root >= FR_ROOTS) RECF_REFUSE("feature root %u, not below %u", root, FR_ROOTS);
        body[2] = body[3] = 0;
        r = get_flag(x, C_FT_SAME);
        if (r == -2) CUT(C_FT_SAME);
        if (r < 0 || (r && !x->sited[root])) RECF_REFUSE("a feature's same-site flag for root %u with no site", root);
        if (r) put32(body + 4, x->site[root]);
        else if (!recf_bytes(&x->rd[C_FT_SITE], body + 4, 4)) CUT(C_FT_SITE);
        x->site[root] = get32(body + 4);
        x->sited[root] = 1;
        n = root_args(root);
        if (!recf_bytes(&x->rd[C_FT_ARGS], body + 8, n)) CUT(C_FT_ARGS);
        blen = 8 + n;
        break;
    }
    case RK_EV_FEATURE_END:
        memset(body, 0, 8);
        if (!recf_bytes(&x->rd[C_FE_ROOT], body, 1)) CUT(C_FE_ROOT);
        if (!recf_varint(&x->rd[C_FE_RESULT], &v)) CUT(C_FE_RESULT);
        put32(body + 4, v);
        blen = 8;
        break;
    case RK_EV_CREATE:
    case RK_EV_NET_CREATE: {
        unsigned n = kind == RK_EV_CREATE ? 28 : (unsigned)sizeof(RecNetCreate);
        if (!get_id(x, &id)) CUT(C_ID);
        memset(body, 0, n);
        put32(body, id);
        if (!recf_bytes(&x->rd[C_CREATE], body + 4, kind == RK_EV_CREATE ? 21 : 26)) CUT(C_CREATE);
        blen = n;
        break;
    }
    case RK_EV_DRAW:
        memset(body, 0, 12);
        if (!recf_bytes(&x->rd[C_DRAW], body, 1) || !recf_bytes(&x->rd[C_DRAW], body + 4, 8)) CUT(C_DRAW);
        blen = 12;
        break;
    case RK_EV_BUILT:
    case RK_EV_STATE:
        if (!get_id(x, &id)) CUT(C_ID);
        memset(body, 0, 8);
        put32(body, id);
        if (!recf_bytes(&x->rd[C_RAW], body + 4, kind == RK_EV_BUILT ? 4 : 1)) CUT(C_RAW);
        blen = 8;
        break;
    case RK_EV_INTERCEPT:
        memset(body, 0, 16);
        if (!recf_bytes(&x->rd[C_RAW], body, 14)) CUT(C_RAW);
        blen = 16;
        break;
    case RK_EV_SLICE:
        if (!get_id(x, &id)) CUT(C_ID);
        put32(body, id);
        blen = 4;
        break;
    default:
        RECF_REFUSE("record kind %u in the events frame", kind);
    }
    *len = 0;
    if (!recf_body_put(out, len, body, blen)) RECF_REFUSE("an event of %u bytes past the body", blen);
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
    return recf_close(((Events*)f)->rd, EV_COLS, "events", why, cap);
}

const RecFrameOps recframe_events = {
    "events", RCS_EVENTS,
    events_make, events_drop, events_restart, NULL, NULL,
#ifdef RECF_TEST_COPY
    events_copy,
#endif
    events_enc_iter, events_enc_rec, events_enc_iter_end, events_enc_write,
    events_dec_open, events_dec_iter, events_dec_rec, events_dec_iter_end, events_dec_close
};
