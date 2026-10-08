/* tagpu_reccodec.c -- the demo recorder's stream codec.

   A stream block may be a SECTIONED CONTAINER (tagpu_recfmt.h, RecSecHead and RCS_*): the
   record kinds as runs, each category's record bodies verbatim in a section of their own, and the
   UNIT FRAME, which carries every RK_UNIT_DELTA as residuals against a prediction. It decodes to
   exactly the records it was made from, so whatever reads a block's payload is unchanged.

   THE UNIT FRAME. The codec keeps a copy of every unit image (L1: the image at the end of the
   previous iteration), the image one iteration before (L2), five lanes of the one before that
   (L3), and per slot and lane which of four candidates predicts the lane. RT_CODEC cuts the
   0x148-byte image into lanes; the rules find the lanes they read by role, never by offset. Each
   iteration the candidates -- the units live at its start that no RK_UNIT or RK_UNIT_GONE names --
   are predicted lane by lane: candidate 0 is the engine's own rule (frame_slot says which and
   where it was read) or the previous value, 1 the previous value, 2 the linear extrapolation,
   3 the turned air velocity. A unit is listed when a lane's value differs from its prediction;
   the winner is then the first candidate closest to the value, which both sides derive from
   decoded values alone. A unit that changes nothing costs nothing; the decoder rebuilds each
   RK_UNIT_DELTA from the units whose prediction made them differ from L1.

   THREE INVARIANTS make this lossless whatever the codec gets wrong:
   - DECODE BEFORE WRITE (an ordering). The writer encodes a block with its encoder, packs it,
     unpacks those bytes and decodes them with a second state through this file's decoder, and
     writes the sectioned block only when that gives back the records byte for byte; otherwise it
     writes the records (reccodec_stream_block). A codec bug costs size, never data.
   - RESET POINTS (an ordering). The frame's state crosses block boundaries only inside a run of
     sectioned blocks. It restarts (RBF_FRAME_RESET) at a segment's first stream block, at the
     first after a snapshot and at the first after a records block, from the copy of the units a
     reader holds there: a reader starts only at a snapshot or a segment's start and reads a block
     only between iterations. A block that continues a frame this decoder did not start is
     refused, never misread.
   - BOUNDS. Every value read from a file is data. Each length, count, slot, varint, lane, role and
     flag is tested against its bound before it is used and a refusal names it; every operand of a
     rule is clamped before it is multiplied, so no int64 product can overflow; the constants a
     header carries (the defs' values, gravity) are clamped where they are used.

   NO FLOATING POINT. The writer thread starts at x87 precision 64 (mingw's TLS callback runs
   `fninit` on every thread it attaches), the engine runs at 53, and the Python reader
   (tools/replay/reccodec.py) must agree bit for bit: every rule is int64 arithmetic with floor
   shifts and named divisions.

   MEMORY: per slot the copy (0x148 bytes), L2 (0x148), L3 (16), the winners (one byte per lane,
   128) and four flags, about 1,150 bytes; per codec the air index (128 KB) and the scratch of the
   largest block met so far. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ZSTD_STATIC_LINKING_ONLY       /* ZSTD_getFrameHeader */
#include "zstd.h"
#include "tagpu_reccodec.h"

#define IMG     REC_UNIT_IMAGE
#define NDW     REC_UNIT_DWORDS
#define L3B     16                /* L3: heading u16, layer u8, 0, vx, vy, vz                       */
#define NOAIR   0xFFFFu
#define MB_MAX  ((RECC_LANES_MAX + 7) / 8)

#define REFUSE(...) do { if (why && cap) snprintf(why, cap, __VA_ARGS__); return 0; } while (0)

/* round(8192 * sin(2 * pi * i / 512)); equal to the engine's table at 0x509F00 on all 512
   entries, none within 0.0016 of a rounding tie (MEASURED 2026-10-07/08 against the retail
   executable). A literal of the formula, never read from the executable. */
static const int16_t SIN[512] = {
        0,   101,   201,   302,   402,   502,   603,   703,   803,   903,  1003,  1102,  1202,  1301,  1401,  1499,
     1598,  1697,  1795,  1893,  1990,  2088,  2185,  2282,  2378,  2474,  2570,  2665,  2760,  2854,  2948,  3042,
     3135,  3228,  3320,  3411,  3503,  3593,  3683,  3773,  3862,  3950,  4038,  4125,  4212,  4297,  4383,  4467,
     4551,  4634,  4717,  4799,  4880,  4960,  5040,  5119,  5197,  5274,  5351,  5427,  5501,  5575,  5649,  5721,
     5793,  5863,  5933,  6002,  6070,  6137,  6203,  6268,  6333,  6396,  6458,  6519,  6580,  6639,  6698,  6755,
     6811,  6867,  6921,  6974,  7027,  7078,  7128,  7177,  7225,  7272,  7317,  7362,  7405,  7448,  7489,  7529,
     7568,  7606,  7643,  7679,  7713,  7746,  7779,  7809,  7839,  7868,  7895,  7921,  7946,  7970,  7993,  8014,
     8035,  8054,  8071,  8088,  8103,  8117,  8130,  8142,  8153,  8162,  8170,  8177,  8182,  8186,  8190,  8191,
     8192,  8191,  8190,  8186,  8182,  8177,  8170,  8162,  8153,  8142,  8130,  8117,  8103,  8088,  8071,  8054,
     8035,  8014,  7993,  7970,  7946,  7921,  7895,  7868,  7839,  7809,  7779,  7746,  7713,  7679,  7643,  7606,
     7568,  7529,  7489,  7448,  7405,  7362,  7317,  7272,  7225,  7177,  7128,  7078,  7027,  6974,  6921,  6867,
     6811,  6755,  6698,  6639,  6580,  6519,  6458,  6396,  6333,  6268,  6203,  6137,  6070,  6002,  5933,  5863,
     5793,  5721,  5649,  5575,  5501,  5427,  5351,  5274,  5197,  5119,  5040,  4960,  4880,  4799,  4717,  4634,
     4551,  4467,  4383,  4297,  4212,  4125,  4038,  3950,  3862,  3773,  3683,  3593,  3503,  3411,  3320,  3228,
     3135,  3042,  2948,  2854,  2760,  2665,  2570,  2474,  2378,  2282,  2185,  2088,  1990,  1893,  1795,  1697,
     1598,  1499,  1401,  1301,  1202,  1102,  1003,   903,   803,   703,   603,   502,   402,   302,   201,   101,
        0,  -101,  -201,  -302,  -402,  -502,  -603,  -703,  -803,  -903, -1003, -1102, -1202, -1301, -1401, -1499,
    -1598, -1697, -1795, -1893, -1990, -2088, -2185, -2282, -2378, -2474, -2570, -2665, -2760, -2854, -2948, -3042,
    -3135, -3228, -3320, -3411, -3503, -3593, -3683, -3773, -3862, -3950, -4038, -4125, -4212, -4297, -4383, -4467,
    -4551, -4634, -4717, -4799, -4880, -4960, -5040, -5119, -5197, -5274, -5351, -5427, -5501, -5575, -5649, -5721,
    -5793, -5863, -5933, -6002, -6070, -6137, -6203, -6268, -6333, -6396, -6458, -6519, -6580, -6639, -6698, -6755,
    -6811, -6867, -6921, -6974, -7027, -7078, -7128, -7177, -7225, -7272, -7317, -7362, -7405, -7448, -7489, -7529,
    -7568, -7606, -7643, -7679, -7713, -7746, -7779, -7809, -7839, -7868, -7895, -7921, -7946, -7970, -7993, -8014,
    -8035, -8054, -8071, -8088, -8103, -8117, -8130, -8142, -8153, -8162, -8170, -8177, -8182, -8186, -8190, -8191,
    -8192, -8191, -8190, -8186, -8182, -8177, -8170, -8162, -8153, -8142, -8130, -8117, -8103, -8088, -8071, -8054,
    -8035, -8014, -7993, -7970, -7946, -7921, -7895, -7868, -7839, -7809, -7779, -7746, -7713, -7679, -7643, -7606,
    -7568, -7529, -7489, -7448, -7405, -7362, -7317, -7272, -7225, -7177, -7128, -7078, -7027, -6974, -6921, -6867,
    -6811, -6755, -6698, -6639, -6580, -6519, -6458, -6396, -6333, -6268, -6203, -6137, -6070, -6002, -5933, -5863,
    -5793, -5721, -5649, -5575, -5501, -5427, -5351, -5274, -5197, -5119, -5040, -4960, -4880, -4799, -4717, -4634,
    -4551, -4467, -4383, -4297, -4212, -4125, -4038, -3950, -3862, -3773, -3683, -3593, -3503, -3411, -3320, -3228,
    -3135, -3042, -2948, -2854, -2760, -2665, -2570, -2474, -2378, -2282, -2185, -2088, -1990, -1893, -1795, -1697,
    -1598, -1499, -1401, -1301, -1202, -1102, -1003,  -903,  -803,  -703,  -603,  -502,  -402,  -302,  -201,  -101,
};

/* The bank rule's Q31 constants: research/sizelab/air-challenge/lib_int/intbank.py's, as
   integers; the format fixes them. */
#define Q31_ONE  INT64_C(2147483648)
#define TH47     INT64_C(13493037705)  /* 2 pi / 65536 * 2^47: a heading step in Q31, << 16      */
#define PI4      INT64_C(1686629713)
#define PI2      INT64_C(3373259426)
#define K47      INT64_C(683565276)    /* 65536 / (2 pi) * 2^16: Q31 radians to heading, << 47   */
#define ATAN_BIG INT64_C(889516847)    /* atan's reduction threshold, about tan(pi / 8) in Q31   */
static const int64_t SINC[8] = { 0, 0, -54, 5918, -426088, 17895697, -357913941,
                                 INT64_C(2147483648) };                          /* x^15 .. x^1 */
static const int64_t COSC[8] = { 0, 4, -592, 53261, -2982616, 89478485, -1073741824,
                                 INT64_C(2147483648) };                          /* x^14 .. 1   */
static const int64_t ATC[15] = { 74051160, -79536431, 85899346, -93368854, 102261126, -113025455,
                                 126322568, -143165577, 165191050, -195225786, 238609294,
                                 -306783378, 429496730, -715827883, INT64_C(2147483648) }; /* w^29 .. w */

/* ---- buffers and varints -------------------------------------------------------------------- */

static int rb_grow(RecBytes* b, size_t more)
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

static void rb_put(RecBytes* b, const void* p, size_t n)
{
    if (n && rb_grow(b, n)) {
        memcpy(b->p + b->n, p, n);
        b->n += n;
    }
}

static void rb_u32(RecBytes* b, uint32_t v) { rb_put(b, &v, 4); }

static void rb_varint(RecBytes* b, uint32_t v)
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

static void rb_free(RecBytes* b)
{
    free(b->p);
    memset(b, 0, sizeof *b);
}

/* empty, its latched failure cleared and its memory kept */
static void rb_clear(RecBytes* b)
{
    b->n = 0;
    b->oom = 0;
}

/* LEB128 of a value below 2^32 from p[*at, end): at most 5 bytes, the fifth at most 0x0F;
   non-minimal forms read. 1, or 0 when it runs past `end` or past those bounds. */
static int varint_get(const unsigned char* p, size_t end, size_t* at, uint32_t* v)
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

static unsigned get16(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t get32(const unsigned char* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* ---- integer arithmetic: every rule's operands, each function's bound beside it ------------- */

/* the signed reading of the low 32 or 16 bits */
static int64_t s32(int64_t v)
{
    uint32_t u = (uint32_t)(uint64_t)v;
    return u >= 0x80000000u ? (int64_t)u - INT64_C(4294967296) : (int64_t)u;
}
static int64_t s16(int64_t v)
{
    unsigned u = (unsigned)((uint64_t)v & 0xFFFF);
    return u >= 0x8000 ? (int64_t)u - 65536 : (int64_t)u;
}
/* a floor shift, defined for negative values too (C leaves >> of a negative to the compiler) */
static int64_t asr(int64_t a, unsigned n) { return a >= 0 ? a >> n : -1 - ((-1 - a) >> n); }
/* floor division, b > 0 */
static int64_t fdiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (a % b != 0 && a < 0) q--;
    return q;
}
static int64_t clip(int64_t v, int64_t b) { return v < -b ? -b : v > b ? b : v; }
/* _allmul then _allshr(16) (0x4E4400, 0x4E43D0): |a * b| < 2^63 at every call */
static int64_t mul_asr16(int64_t a, int64_t b) { return s32(asr(a * b, 16)); }
static int64_t mulq(int64_t a, int64_t b) { return asr(a * b, 31); }

/* the exact floor square root, digit by digit */
static uint64_t isqrt64(uint64_t n)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static unsigned bitlen(uint64_t a)
{
    unsigned n = 0;
    while (a) {
        n++;
        a >>= 1;
    }
    return n;
}

/* sqrt(S) rounded to nearest-even at 24 significant bits, binary32's significand, as mant *
   2^e: S < 2^62, and the scaled T < 2^52 whenever it is scaled. */
static void round24_sqrt(uint64_t S, int64_t* mant, int* e)
{
    unsigned b = bitlen(isqrt64(S)), k = b < 26 ? 26 - b : 0, n, sh;
    uint64_t T = S << (2 * k), r = isqrt64(T), m, rem, half;
    int exact = r * r == T, up, ov;
    n = bitlen(r);
    sh = n > 24 ? n - 24 : 0;
    m = r >> sh;
    rem = r - (m << sh);
    half = sh ? (uint64_t)1 << (sh - 1) : 0;
    up = sh && (rem > half || (rem == half && (!exact || (m & 1))));
    m += (uint64_t)up;
    ov = (m >> 24) != 0;
    if (ov) m >>= 1;
    *mant = (int64_t)m;
    *e = (int)sh + ov - (int)k;
}

/* 0x4B70EF and 0x4B7123: the table entry at the heading, times e, rounded off 13 bits, low dword
   (DISASSEMBLED: a sine and a cosine off the one table). |SIN * e| <= 2^13 * 2^31. */
static int64_t sinmul(uint32_t h, int64_t e)
{
    return s32(asr((int64_t)SIN[((h + 0x20) >> 7) & 511] * e + 0x1000, 13));
}
static int64_t cosmul(uint32_t h, int64_t e)
{
    return s32(asr((int64_t)SIN[((h + 0x4020) >> 7) & 511] * e + 0x1000, 13));
}

/* an RT_AIR row's constants, clamped where they are used: they are data */
typedef struct AirK { int64_t maxv, brake, acc, bank, pitch, sight_h; } AirK;

static void air_k(const RecAirRow* r, AirK* a)
{
    a->maxv = r->maxv < 1 ? 1 : r->maxv > (1 << 24) ? (1 << 24) : r->maxv;
    a->brake = r->brake < 0 ? 0 : r->brake > (1 << 24) ? (1 << 24) : r->brake;
    a->acc = r->acc < 0 ? 0 : r->acc > (1 << 24) ? (1 << 24) : r->acc;
    a->bank = clip(r->bank, 1 << 24);
    a->pitch = clip(r->pitch, 1 << 24);
    a->sight_h = r->sight_h;
}

/* The air step's known part, its damping and drift limit (0x43D2F1..0x43D47D; acc def +0x19E and
   maxvelocity +0x192 at 0x43D2F1..0x43D326, the `_allshl` 0x4E44F0 by 16 then `_alldiv`), from
   the velocity and heading the step starts with. The engine compares |v1| as a binary32 with the
   brake rate: round24_sqrt makes that compare and the rescale exact rationals. Bounds: v is s32
   and |d| <= 2^17, so v * d < 2^48; v1x, v1z clipped to 2^30, so the sum of squares < 2^61; the
   compare shifts the smaller side, both below 2^51; kf < 2^16. */
static void known(const AirK* a, uint32_t h, int64_t vx, int64_t vy, int64_t vz, int64_t* ox,
                  int64_t* oy, int64_t* oz)
{
    int64_t d = 0x10000 - a->acc * 65536 / a->maxv, v1x, v1z, m, kf, ex;
    int e, over;
    if (d < -(INT64_C(1) << 17)) d = -(INT64_C(1) << 17);
    v1x = clip(mul_asr16(vx, d), 1 << 30);
    v1z = clip(mul_asr16(vz, d), 1 << 30);
    *oy = mul_asr16(vy, d);
    round24_sqrt((uint64_t)(v1x * v1x + v1z * v1z), &m, &e);
    over = e >= 0 ? (m << e) > a->brake : m > (a->brake << -e);
    if (!over) {
        *ox = v1x;
        *oz = v1z;
        return;
    }
    kf = s32(e >= 0 ? a->brake * 65536 / (m << e) : (a->brake * 65536 << -e) / m);
    ex = s32((e >= 0 ? m << e : m >> -e) - a->brake);
    *ox = s32(mul_asr16(v1x, kf) - sinmul(h, ex));
    *oz = s32(mul_asr16(v1z, kf) - cosmul(h, ex));
}

/* sin and cos of a heading in Q31: integer reduction to the octant, then Taylor series. x < 2^31,
   so every Q31 product < 2^62. */
static void sin_cos_q31(uint32_t h, int64_t* sn, int64_t* cs)
{
    int64_t r, x, x2, s, c;
    unsigned n, i;
    h &= 0xFFFF;
    n = ((h + 0x2000) >> 14) & 3;
    r = (int64_t)((h + 0x2000) & 0x3FFF) - 0x2000;
    x = asr(r * TH47, 16);
    x2 = mulq(x, x);
    s = SINC[0];
    for (i = 1; i < 8; i++) s = mulq(s, x2) + SINC[i];
    s = mulq(s, x);
    c = COSC[0];
    for (i = 1; i < 8; i++) c = mulq(c, x2) + COSC[i];
    switch (n) {
    case 0: *sn = s; *cs = c; break;
    case 1: *sn = c; *cs = -s; break;
    case 2: *sn = -s; *cs = -c; break;
    default: *sn = -c; *cs = s; break;
    }
}

/* atan(u / 2^31) in Q31 for 0 <= u <= 2^31; (u - 2^31) * 2^31 >= -2^62 is a multiplication,
   never a shift of a negative. */
static int64_t atan_q31(int64_t u)
{
    int big = u > ATAN_BIG;
    int64_t w = big ? fdiv((u - Q31_ONE) * Q31_ONE, u + Q31_ONE) : u, w2, a;
    unsigned i;
    w2 = mulq(w, w);
    a = ATC[0];
    for (i = 1; i < 15; i++) a = mulq(a, w2) + ATC[i];
    a = mulq(a, w);
    return big ? PI4 + a : a;
}

/* 0x43D0D0's bank: the smoothed acceleration's x turned by the heading (skipped at heading 0),
   times the def's scale >> 16, then atan2 against q in heading units -- in Q31 integers
   (intbank.py exactly). ax, az clipped to 2^30 and |scale| <= 2^24, so -x' * scale < 2^56;
   q < 2^29 and |val| <= 2^31, so num * 2^31 <= 2^62. */
static int64_t bank_int(int64_t ax, int64_t az, uint32_t h, int64_t scale, int64_t q)
{
    int64_t sn, cs, xr, val, a, num, den, u, ang;
    int swap;
    ax = clip(ax, 1 << 30);
    az = clip(az, 1 << 30);
    sin_cos_q31(h, &sn, &cs);
    xr = (h & 0xFFFF) == 0 ? ax : asr(ax * cs - az * sn + (INT64_C(1) << 30), 31);
    val = s32(asr(-xr * scale, 16));
    a = val < 0 ? -val : val;
    swap = a > q;
    num = swap ? q : a;
    den = swap ? a : q;
    u = num * Q31_ONE / (den > 1 ? den : 1);
    ang = atan_q31(u);
    if (swap) ang = PI2 - ang;
    if (val < 0) ang = -ang;
    return asr(ang * K47 + (INT64_C(1) << 46), 47);
}

/* A1 turned by the angle from A2 to A1: A1 * (A1 conj A2) / |A2|^2, rounded half up by a floor
   division; operands clipped to 2^16, so every product < 2^51. */
static void rot(int64_t x1, int64_t z1, int64_t x2, int64_t z2, int64_t* px, int64_t* pz)
{
    int64_t n2, dot, cr;
    x1 = clip(x1, 1 << 16);
    z1 = clip(z1, 1 << 16);
    x2 = clip(x2, 1 << 16);
    z2 = clip(z2, 1 << 16);
    n2 = x2 * x2 + z2 * z2;
    if (n2 > 0 && (x1 || z1)) {
        dot = x1 * x2 + z1 * z2;
        cr = x2 * z1 - z2 * x1;
        *px = fdiv(2 * (x1 * dot - z1 * cr) + n2, 2 * n2);
        *pz = fdiv(2 * (x1 * cr + z1 * dot) + n2, 2 * n2);
    } else {
        *px = x1;
        *pz = z1;
    }
}

/* ---- the state ------------------------------------------------------------------------------ */

struct RecCodec {
    unsigned nslots;
    int valid;
    RecCodecParams p;                     /* the restart's, copied: the codec owns what it reads */
    int lane_of[REC_LR_N];                /* the lane of each role but PLAIN and COUNT           */
    unsigned char s1[RECC_LANES_MAX], s2[RECC_LANES_MAX], cnt[RECC_COUNTS_MAX];
    unsigned ns1, ns2, ncnt;
    unsigned char defwin[RECC_LANES_MAX];
    int64_t q;                            /* (gravity << 16) / 0xCCD (0x43D186..0x43D1A4)       */
    uint16_t* airix;                      /* type -> RT_AIR row, NOAIR for none                  */
    uint32_t* unit;                       /* the copy: L1                                       */
    uint32_t* l2;
    unsigned char *l3, *win, *age, *rest, *live, *touched;
    RecBytes tl, dl, runs, lslot, lmask, lzz, ids, hash, cols;
    RecBytes sec[RCS_COUNT];              /* the encoder's sections                             */
    unsigned lcount[RECC_TICKS_MAX];
    int over;                             /* the encoder's frame passed RECC_FRAME_CAP          */
};

RecCodec* reccodec_new(unsigned nslots)
{
    RecCodec* c;
    if (nslots < 2 || nslots > 0xFFFF) return NULL;
    c = (RecCodec*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->nslots = nslots;
    c->airix = (uint16_t*)malloc(65536 * sizeof *c->airix);
    c->unit = (uint32_t*)calloc(nslots, IMG);
    c->l2 = (uint32_t*)calloc(nslots, IMG);
    c->l3 = (unsigned char*)calloc(nslots, L3B);
    c->win = (unsigned char*)calloc(nslots, RECC_LANES_MAX);
    c->age = (unsigned char*)calloc(nslots, 1);
    c->rest = (unsigned char*)calloc(nslots, 1);
    c->live = (unsigned char*)calloc(nslots, 1);
    c->touched = (unsigned char*)calloc(nslots, 1);
    if (!c->airix || !c->unit || !c->l2 || !c->l3 || !c->win || !c->age || !c->rest || !c->live ||
        !c->touched) {
        reccodec_free(c);
        return NULL;
    }
    return c;
}

void reccodec_free(RecCodec* c)
{
    unsigned i;
    if (!c) return;
    free(c->airix);
    free(c->unit);
    free(c->l2);
    free(c->l3);
    free(c->win);
    free(c->age);
    free(c->rest);
    free(c->live);
    free(c->touched);
    rb_free(&c->tl);
    rb_free(&c->dl);
    rb_free(&c->runs);
    rb_free(&c->lslot);
    rb_free(&c->lmask);
    rb_free(&c->lzz);
    rb_free(&c->ids);
    rb_free(&c->hash);
    rb_free(&c->cols);
    for (i = 0; i < RCS_COUNT; i++) rb_free(&c->sec[i]);
    free(c);
}

void reccodec_invalidate(RecCodec* c) { c->valid = 0; }
int reccodec_valid(const RecCodec* c) { return c->valid; }
const uint32_t* reccodec_units(const RecCodec* c) { return c->unit; }
const unsigned char* reccodec_live(const RecCodec* c) { return c->live; }

static uint32_t lane_get(const unsigned char* img, const RecLaneRow* r)
{
    const unsigned char* q = img + r->off;
    return r->width == 1 ? q[0] : r->width == 2 ? get16(q) : get32(q);
}

static void lane_put(unsigned char* img, const RecLaneRow* r, uint32_t v)
{
    unsigned char* q = img + r->off;
    q[0] = (unsigned char)v;
    if (r->width > 1) q[1] = (unsigned char)(v >> 8);
    if (r->width > 2) {
        q[2] = (unsigned char)(v >> 16);
        q[3] = (unsigned char)(v >> 24);
    }
}

/* the five lanes of an image the air rules read from L3 */
static void l3_of(const RecCodec* c, const unsigned char* img, unsigned char* out)
{
    uint32_t h = lane_get(img, &c->p.lane[c->lane_of[REC_LR_HEADING]]);
    memset(out, 0, L3B);
    out[0] = (unsigned char)h;
    out[1] = (unsigned char)(h >> 8);
    out[2] = (unsigned char)lane_get(img, &c->p.lane[c->lane_of[REC_LR_LAYER]]);
    put32(out + 4, lane_get(img, &c->p.lane[c->lane_of[REC_LR_VX]]));
    put32(out + 8, lane_get(img, &c->p.lane[c->lane_of[REC_LR_VY]]));
    put32(out + 12, lane_get(img, &c->p.lane[c->lane_of[REC_LR_VZ]]));
}

/* A slot the iteration's records name: a creation, a type change or a correction starts a fresh
   history; a death ends it. */
static void slot_fresh(RecCodec* c, unsigned s)
{
    unsigned char* u = (unsigned char*)(c->unit + (size_t)s * NDW);
    c->rest[s] = 0;
    if (!c->live[s]) {
        c->age[s] = 0;
        return;
    }
    memcpy(c->l2 + (size_t)s * NDW, u, IMG);
    l3_of(c, u, c->l3 + (size_t)s * L3B);
    memcpy(c->win + (size_t)s * RECC_LANES_MAX, c->defwin, RECC_LANES_MAX);
    c->age[s] = 1;
}

void reccodec_restart(RecCodec* c, const RecCodecParams* p, const uint32_t* units,
                      const unsigned char* live)
{
    unsigned s, j, i;
    c->valid = 0;
    /* A failed allocation latches its buffer's oom (rb_grow); some uses reset only its length, so
       a latch would decline every later block of a writer and refuse every later sectioned block
       of a reader. A restart is where both start again after a failure, so every working buffer
       starts here empty and unlatched. */
    rb_clear(&c->tl);
    rb_clear(&c->dl);
    rb_clear(&c->runs);
    rb_clear(&c->lslot);
    rb_clear(&c->lmask);
    rb_clear(&c->lzz);
    rb_clear(&c->ids);
    rb_clear(&c->hash);
    rb_clear(&c->cols);
    for (i = 0; i < RCS_COUNT; i++) rb_clear(&c->sec[i]);
    if (!p->usable || p->nslots != c->nslots || p->nlanes < 1 || p->nlanes > RECC_LANES_MAX ||
        p->nair > RECC_AIR_MAX)
        return;
    if (p != &c->p) c->p = *p;
    for (i = 0; i < REC_LR_N; i++) c->lane_of[i] = -1;
    c->ns1 = c->ns2 = c->ncnt = 0;
    for (j = 0; j < c->p.nlanes; j++) {
        unsigned role = c->p.lane[j].role;
        if (role != REC_LR_PLAIN && role != REC_LR_COUNT_ && role < REC_LR_N) c->lane_of[role] = (int)j;
        /* the default winner: the rule where the role has one, else the previous value */
        c->defwin[j] = role == REC_LR_PLAIN || role == REC_LR_TURN || role == REC_LR_FW ||
                       role == REC_LR_FH || role == REC_LR_LAYER || role == REC_LR_TYPE;
        if (c->defwin[j]) c->s1[c->ns1++] = (unsigned char)j;
        else if (role == REC_LR_VY || role == REC_LR_SPEED || role == REC_LR_AX ||
                 role == REC_LR_AY || role == REC_LR_AZ || role == REC_LR_BANK ||
                 role == REC_LR_ROLL || role == REC_LR_Y || role == REC_LR_SZ)
            c->s2[c->ns2++] = (unsigned char)j;
        else if (role == REC_LR_COUNT_ && c->ncnt < RECC_COUNTS_MAX)
            c->cnt[c->ncnt++] = (unsigned char)j;
    }
    for (i = 1; i < REC_LR_COUNT_; i++)
        if (c->lane_of[i] < 0) return;          /* reccodec_params refuses such a table */
    memset(c->defwin + c->p.nlanes, 1, RECC_LANES_MAX - c->p.nlanes);
    {
        int64_t g = c->p.gravity < 0 ? 0 : c->p.gravity > (1 << 24) ? (1 << 24) : c->p.gravity;
        c->q = g * 65536 / 0xCCD;
    }
    for (i = 0; i < 65536; i++) c->airix[i] = NOAIR;
    for (i = 0; i < c->p.nair; i++) c->airix[c->p.air[i].type] = (uint16_t)i;
    if (units != c->unit) memmove(c->unit, units, (size_t)c->nslots * IMG);
    if (live != c->live) memmove(c->live, live, c->nslots);
    for (s = 0; s < c->nslots; s++) {
        unsigned char* u = (unsigned char*)(c->unit + (size_t)s * NDW);
        c->live[s] = c->live[s] != 0;
        if (!c->live[s]) memset(u, 0, IMG);
        c->touched[s] = 0;
        c->rest[s] = 0;
        memcpy(c->l2 + (size_t)s * NDW, u, IMG);
        l3_of(c, u, c->l3 + (size_t)s * L3B);
        memcpy(c->win + (size_t)s * RECC_LANES_MAX, c->defwin, RECC_LANES_MAX);
        c->age[s] = c->live[s];
    }
    c->valid = 1;
}

/* ---- the header's codec entries ------------------------------------------------------------- */

int reccodec_params(RecCodecParams* p, unsigned nslots, const void* codec, unsigned ncodec,
                    const void* physics, unsigned nphysics, const void* air, unsigned nair,
                    char* why, unsigned cap)
{
    memset(p, 0, sizeof *p);
    p->nslots = nslots;
    if (nslots < 2 || nslots > 0xFFFF) REFUSE("RT_SLOTS %u is outside 2..65535", nslots);
    if (codec) {
        const unsigned char* b = (const unsigned char*)codec;
        unsigned n, i, at = 0, counts = 0;
        uint32_t seen = 0;
        if (ncodec < 4) REFUSE("RT_CODEC is %u bytes, under its 4-byte head", ncodec);
        n = get16(b);
        if (get16(b + 2)) REFUSE("RT_CODEC's pad is not zero");
        if (n < 1 || n > RECC_LANES_MAX) REFUSE("RT_CODEC has %u rows, not 1..%u", n, RECC_LANES_MAX);
        if (ncodec != 4 + 4 * n) REFUSE("RT_CODEC is %u bytes, not %u for %u rows", ncodec, 4 + 4 * n, n);
        for (i = 0; i < n; i++) {
            const unsigned char* r = b + 4 + 4 * i;
            unsigned off = get16(r), width = r[2], role = r[3];
            if (off != at) REFUSE("RT_CODEC row %u starts at %#x, not %#x: the rows must tile the image", i, off, at);
            if (width != 1 && width != 2 && width != 4) REFUSE("RT_CODEC row %u is %u bytes wide, not 1, 2 or 4", i, width);
            if (at + width > IMG) REFUSE("RT_CODEC row %u runs past the image's %#x bytes", i, IMG);
            if (role >= REC_LR_N) REFUSE("RT_CODEC row %u has role %u, past %u", i, role, REC_LR_N - 1);
            if (rec_role_width[role] && width != rec_role_width[role])
                REFUSE("RT_CODEC row %u: role %u is %u bytes wide, not %u", i, role, rec_role_width[role], width);
            if (role == REC_LR_COUNT_ && ++counts > RECC_COUNTS_MAX)
                REFUSE("RT_CODEC row %u is COUNT row %u, at most %u", i, counts, RECC_COUNTS_MAX);
            if (role != REC_LR_PLAIN && role != REC_LR_COUNT_) {
                if (seen >> role & 1) REFUSE("RT_CODEC row %u repeats role %u", i, role);
                seen |= (uint32_t)1 << role;
            }
            p->lane[i].off = (uint16_t)off;
            p->lane[i].width = (uint8_t)width;
            p->lane[i].role = (uint8_t)role;
            at += width;
        }
        if (at != IMG) REFUSE("RT_CODEC's rows cover %#x bytes, not the image's %#x", at, IMG);
        for (i = 1; i < REC_LR_COUNT_; i++)
            if (!(seen >> i & 1)) REFUSE("RT_CODEC has no row for role %u", i);
        p->nlanes = n;
    }
    if (physics) {
        const unsigned char* b = (const unsigned char*)physics;
        if (nphysics != 8) REFUSE("RT_PHYSICS is %u bytes, not 8", nphysics);
        if (b[5] > 1) REFUSE("RT_PHYSICS's sight rule is %u, not 0 or 1", b[5]);
        if (get16(b + 6)) REFUSE("RT_PHYSICS's pad is not zero");
        p->gravity = (int32_t)get32(b);
        p->sea = b[4];
        p->sight = b[5];
    }
    if (air) {
        const unsigned char* b = (const unsigned char*)air;
        uint32_t n;
        unsigned i;
        if (nair < 8) REFUSE("RT_AIR is %u bytes, under its 8-byte head", nair);
        n = get32(b);
        if (get32(b + 4)) REFUSE("RT_AIR's pad is not zero");
        if (n > RECC_AIR_MAX) REFUSE("RT_AIR has %lu rows, past %u", (unsigned long)n, RECC_AIR_MAX);
        if (nair != 8 + 24 * n) REFUSE("RT_AIR is %u bytes, not %lu for %lu rows", nair,
                                       (unsigned long)(8 + 24 * n), (unsigned long)n);
        for (i = 0; i < n; i++) {
            const unsigned char* r = b + 8 + 24 * i;
            RecAirRow* a = &p->air[i];
            a->type = (uint16_t)get16(r);
            a->sight_h = r[2];
            a->zero = r[3];
            if (a->zero) REFUSE("RT_AIR row %u's pad is not zero", i);
            if (i && a->type <= p->air[i - 1].type)
                REFUSE("RT_AIR row %u's type %u does not follow %u", i, a->type, p->air[i - 1].type);
            a->maxv = (int32_t)get32(r + 4);
            a->brake = (int32_t)get32(r + 8);
            a->acc = (int32_t)get32(r + 12);
            a->bank = (int32_t)get32(r + 16);
            a->pitch = (int32_t)get32(r + 20);
        }
        p->nair = n;
    }
    p->usable = codec && physics;
    return 1;
}

/* ---- one slot of one iteration -------------------------------------------------------------- */

typedef struct Eval {
    RecCodec* c;
    const unsigned char* l1;            /* the copy: the image at the end of the last iteration */
    const unsigned char* l2;
    const unsigned char* l3;
    unsigned char* win;
    unsigned char u[IMG];               /* U: this iteration's image, built lane by lane       */
    uint32_t gt;
    unsigned age;
    int dec;
    /* the decoder's residuals: the listed unit's mask (NULL: not listed) and the columns */
    const unsigned char* mask;
    const unsigned char* sec;
    size_t* colat;
    size_t secend;
    /* the encoder's: zigzag residuals by lane and their mask */
    unsigned char rmask[MB_MAX];
    uint32_t zz[RECC_LANES_MAX];
    int any;
} Eval;

static uint32_t err_of(uint32_t a, uint32_t c, uint32_t m)
{
    uint32_t d1 = (a - c) & m, d2 = (c - a) & m;
    return d1 < d2 ? d1 : d2;
}

/* One lane: predict it by its winner among the candidates, take the residual (encoder) or apply
   it (decoder), and choose the next winner -- the first candidate closest to the value. `rule`
   0 is a generic step: candidate 0 is then the previous value and never chosen. */
static int step(Eval* x, unsigned j, int rule, uint32_t c0, int has3, uint32_t c3)
{
    const RecLaneRow* r = &x->c->p.lane[j];
    uint32_t m = r->width == 4 ? 0xFFFFFFFFu : ((1u << (8 * r->width)) - 1);
    uint32_t l1 = lane_get(x->l1, r), l2 = lane_get(x->l2, r), cand[4], act, rr;
    unsigned w = x->win[j] & 3, i, best;
    cand[0] = rule ? c0 & m : l1;
    cand[1] = l1;
    cand[2] = (2u * l1 - l2) & m;
    cand[3] = has3 ? c3 & m : cand[2];
    if (x->dec) {
        rr = 0;
        if (x->mask && (x->mask[j >> 3] >> (j & 7) & 1)) {
            uint32_t z;
            int64_t sr;
            if (!varint_get(x->sec, x->secend, &x->colat[j], &z)) return 0;
            sr = (int64_t)(z >> 1) ^ -(int64_t)(z & 1);
            rr = (uint32_t)(uint64_t)sr & m;
        }
        act = (cand[w] + rr) & m;
        lane_put(x->u, r, act);
    } else {
        act = lane_get(x->u, r);
        rr = (act - cand[w]) & m;
        if (rr) {
            uint64_t half = ((uint64_t)m + 1) >> 1;
            x->zz[j] = rr < half ? 2 * rr : (uint32_t)(2 * ((uint64_t)m + 1 - rr) - 1);
            x->rmask[j >> 3] |= (unsigned char)(1u << (j & 7));
            x->any = 1;
        }
    }
    /* Generic early-out, exact: with the previous value the winner, L1 == L2 and no residual,
       every candidate equals the value and the first of them, the previous value, stays. */
    if (!rule && w == 1 && l1 == l2 && act == l1) return 1;
    best = rule ? 0 : 1;
    for (i = best + 1; i < 4; i++)
        if (err_of(act, cand[i], m) < err_of(act, cand[best], m)) best = i;
    x->win[j] = (unsigned char)best;
    return 1;
}

#define LANE(role)  (&c->p.lane[c->lane_of[role]])
#define U_(role)    lane_get(x->u, LANE(role))
#define L1_(role)   lane_get(x->l1, LANE(role))
#define L2_(role)   lane_get(x->l2, LANE(role))
#define STEP(role, rl, c0, h3, c3) do { if (!step(x, (unsigned)c->lane_of[role], rl, c0, h3, c3)) return 0; } while (0)

/* The steps of one slot, in an order where each rule reads only lanes decoded before it:
   S1 every row's generic lanes; S2 a ground row's generic lanes that an aircraft predicts by
   rule; S3 the heading; S4 the velocity; S5 an aircraft's speed, acceleration and bank; S6 the
   position; S7 the cells, the stamps and the countdowns. 1, or 0 when a residual cannot be read. */
static int frame_slot(Eval* x)
{
    RecCodec* c = x->c;
    unsigned i, ai, layer, layer1;
    int air, airborne, landing, sight = c->p.sight;
    int64_t vx, vy, vz;
    for (i = 0; i < c->ns1; i++)
        if (!step(x, c->s1[i], 0, 0, 0, 0)) return 0;
    ai = c->airix[U_(REC_LR_TYPE)];
    air = ai != NOAIR && ai < c->p.nair;
    if (!air)
        for (i = 0; i < c->ns2; i++)
            if (!step(x, c->s2[i], 0, 0, 0, 0)) return 0;
    /* S3: heading += turn rate (0x43CFD1..0x43CFDB, DISASSEMBLED; research/sizelab/units) */
    STEP(REC_LR_HEADING, 1, L1_(REC_LR_HEADING) + U_(REC_LR_TURN), 0, 0);
    layer = U_(REC_LR_LAYER) & 3;
    layer1 = L1_(REC_LR_LAYER) & 3;
    airborne = layer == 2;
    landing = layer1 == 2 && layer == 1;
    if (air) {
        /* S4 and S5, an aircraft: def +0x241 bit 11 sends its move to the air step 0x43D290
           (0x43DD36..0x43DD44, DISASSEMBLED). The velocity is the step's known part from L1 plus
           its steering term, unknown here, estimated as A1 = L1.v - known(L2) continued (C0) or
           turned by the angle from A2 = L2.v - known(L3) (C3); when vx missed, vz completes a
           steering term saturated at |A| = acc; vy continues a saturated climb at the limit the
           vertical clamp takes from the previous tick's speed. MEASURED: these rules decode the
           air war's 859,616 unit images exactly and leave 1.34 MiB of 6.2
           (research/sizelab/air, air-challenge). Bounds: k, A1, A2 are s32, acc <= 2^24, so
           acc^2 - ax^2 lies within 2^62. */
        AirK a;
        int64_t kx, kz, qx, qz, rx, rz, t, a1x = 0, a1z = 0, a2x = 0, a2z = 0, px, pz;
        int64_t sp1, sp2, cl1, cl2, vy1, c0;
        int ok1, ok2;
        air_k(&c->p.air[ai], &a);
        known(&a, L1_(REC_LR_HEADING), s32(L1_(REC_LR_VX)), s32(L1_(REC_LR_VY)),
              s32(L1_(REC_LR_VZ)), &kx, &t, &kz);
        ok1 = x->age >= 2 && layer1 == 2 && (L2_(REC_LR_LAYER) & 3) == 2;
        ok2 = ok1 && x->age >= 3 && (x->l3[2] & 3) == 2;
        if (ok1) {
            known(&a, L2_(REC_LR_HEADING), s32(L2_(REC_LR_VX)), s32(L2_(REC_LR_VY)),
                  s32(L2_(REC_LR_VZ)), &qx, &t, &qz);
            a1x = s32(s32(L1_(REC_LR_VX)) - qx);
            a1z = s32(s32(L1_(REC_LR_VZ)) - qz);
        }
        if (ok2) {
            known(&a, get16(x->l3), s32(get32(x->l3 + 4)), s32(get32(x->l3 + 8)),
                  s32(get32(x->l3 + 12)), &rx, &t, &rz);
            a2x = s32(s32(L2_(REC_LR_VX)) - rx);
            a2z = s32(s32(L2_(REC_LR_VZ)) - rz);
        }
        rot(a1x, a1z, a2x, a2z, &px, &pz);
        STEP(REC_LR_VX, 1, airborne ? (uint32_t)(uint64_t)s32(kx + a1x) : 0, 1,
             airborne ? (uint32_t)(uint64_t)s32(kx + px) : 0);
        c0 = 0;
        if (airborne) {
            int64_t ax = s32(s32(U_(REC_LR_VX)) - kx);
            if (ax == a1x) {
                c0 = s32(kz + a1z);
            } else {
                int64_t rem = a.acc * a.acc - ax * ax;
                int64_t root = (int64_t)isqrt64((uint64_t)(rem > 0 ? rem : 0));
                c0 = s32(kz + (a1z >= 0 ? root : -root));
            }
        }
        STEP(REC_LR_VZ, 1, (uint32_t)(uint64_t)c0, 1, airborne ? (uint32_t)(uint64_t)s32(kz + pz) : 0);
        sp1 = s32(L1_(REC_LR_SPEED));
        sp2 = s32(L2_(REC_LR_SPEED));
        cl1 = (sp1 & ~(int64_t)3) < 0x40000 ? 0x10000 : asr(sp1, 2);
        cl2 = (sp2 & ~(int64_t)3) < 0x40000 ? 0x10000 : asr(sp2, 2);
        vy1 = s32(L1_(REC_LR_VY));
        c0 = airborne && x->age >= 2 ? (vy1 == cl2 ? cl1 : vy1 == -cl2 ? -cl1 : 0) : 0;
        STEP(REC_LR_VY, 1, (uint32_t)(uint64_t)c0, 0, 0);
        /* speed = trunc(sqrt(v.v)) (the air step's fsqrt, exact while |v| < 2^25, DISASSEMBLED);
           each axis clipped to 2^30, so v.v < 2^62 */
        vx = clip(s32(U_(REC_LR_VX)), 1 << 30);
        vy = clip(s32(U_(REC_LR_VY)), 1 << 30);
        vz = clip(s32(U_(REC_LR_VZ)), 1 << 30);
        STEP(REC_LR_SPEED, 1, airborne ? (uint32_t)isqrt64((uint64_t)(vx * vx + vy * vy + vz * vz)) : 0, 0, 0);
        /* the smoothed acceleration A = A * 0xF333 >> 16 + dv (0x43D0D0, DISASSEMBLED); a landing
           (0x43D210(1), DISASSEMBLED, never yet in a recording) calls it with dv = 0 */
        {
            static const int ar[3] = { REC_LR_AX, REC_LR_AY, REC_LR_AZ };
            static const int vr[3] = { REC_LR_VX, REC_LR_VY, REC_LR_VZ };
            for (i = 0; i < 3; i++) {
                int64_t l1a = s32(L1_(ar[i])), v;
                v = airborne ? mul_asr16(l1a, 0xF333) + s32(U_(vr[i])) - s32(L1_(vr[i]))
                    : landing ? mul_asr16(l1a, 0xF333) : l1a;
                STEP(ar[i], 1, (uint32_t)(uint64_t)v, 0, 0);
            }
        }
        /* the bank, 0x43D0D0's atan2 of the turned x' (0x43D16A..0x43D1F6), and its pitch-scale
           twin from the same x' (0x43D1A0, 0x43D1D5), DISASSEMBLED; bank_int matches the engine on
           588,451 of 588,452 airborne iterations (MEASURED, research/sizelab/air-challenge) */
        STEP(REC_LR_BANK, 1, airborne || landing
             ? (uint32_t)(uint64_t)bank_int(s32(U_(REC_LR_AX)), s32(U_(REC_LR_AZ)), U_(REC_LR_HEADING), a.bank, c->q)
             : L1_(REC_LR_BANK), 0, 0);
        STEP(REC_LR_ROLL, 1, airborne || landing
             ? (uint32_t)(uint64_t)bank_int(s32(U_(REC_LR_AX)), s32(U_(REC_LR_AZ)), U_(REC_LR_HEADING), a.pitch, c->q)
             : L1_(REC_LR_ROLL), 0, 0);
    } else {
        /* S4, a ground mover: v = -(sin, cos)(heading) * speed (0x4B70EF, 0x4B7123, DISASSEMBLED;
           research/sizelab/units); |SIN * speed| < 2^44 */
        uint32_t h = U_(REC_LR_HEADING);
        int64_t sp = s32(U_(REC_LR_SPEED));
        STEP(REC_LR_VX, 1, (uint32_t)(uint64_t)-asr((int64_t)SIN[((h + 0x20) >> 7) & 511] * sp + 0x1000, 13), 0, 0);
        STEP(REC_LR_VZ, 1, (uint32_t)(uint64_t)-asr((int64_t)SIN[((h + 0x4020) >> 7) & 511] * sp + 0x1000, 13), 0, 0);
    }
    /* S6: position += velocity (0x43D7FF..0x43D839, y at 0x43D80D an aircraft's only,
       DISASSEMBLED) */
    STEP(REC_LR_X, 1, L1_(REC_LR_X) + U_(REC_LR_VX), 0, 0);
    STEP(REC_LR_Z, 1, L1_(REC_LR_Z) + U_(REC_LR_VZ), 0, 0);
    if (air) STEP(REC_LR_Y, 1, L1_(REC_LR_Y) + U_(REC_LR_VY), 0, 0);
    /* S7: the footprint's cell (x - w * 2^19 + 0x80000) >> 20 (0x43D86F..0x43D895, DISASSEMBLED);
       |w * 2^19| <= 2^34 */
    STEP(REC_LR_FX, 1, (uint32_t)(uint64_t)asr(s32(U_(REC_LR_X)) - s16(U_(REC_LR_FW)) * 524288 + 0x80000, 20), 0, 0);
    STEP(REC_LR_FZ, 1, (uint32_t)(uint64_t)asr(s32(U_(REC_LR_Z)) - s16(U_(REC_LR_FH)) * 524288 + 0x80000, 20), 0, 0);
    /* the sight cell x >> 21 with LosType bit 2 set (0x4825B0: the test 0x4825BA..0x4825CE, the
       cells 0x4825D4..0x48261C, DISASSEMBLED); with it clear the engine takes 0x4826B7's
       truncating division and table, which no rule predicts: RT_PHYSICS's sight 0 makes the two
       cells generic */
    STEP(REC_LR_SX, sight, (uint32_t)(uint64_t)asr(s32(U_(REC_LR_X)), 21), 0, 0);
    /* the claim stamp: GameTime when the footprint or the layer moved (0x440A83..0x440A91 writes
       main+0x38A47 as a dword, DISASSEMBLED); the iteration labelled gt holds step gt - 1 */
    {
        int moved = U_(REC_LR_FX) != L1_(REC_LR_FX) || U_(REC_LR_FZ) != L1_(REC_LR_FZ) || layer != layer1;
        STEP(REC_LR_CLAIM, 1, moved ? x->gt - 1u : L1_(REC_LR_CLAIM), 0, 0);
    }
    /* the move stamp: GameTime when it moved or changed layer (0x43D85A..0x43D865, DISASSEMBLED) */
    {
        int moved = U_(REC_LR_VX) || U_(REC_LR_VY) || U_(REC_LR_VZ) || layer != layer1;
        STEP(REC_LR_STAMP, 1, moved ? x->gt - 1u : L1_(REC_LR_STAMP), 0, 0);
    }
    /* the countdowns max(v - 1, 0) (the reload 0x49E1C8, +0xFA and +0xFB 0x48ADF0, DISASSEMBLED) */
    for (i = 0; i < c->ncnt; i++) {
        uint32_t v = lane_get(x->l1, &c->p.lane[c->cnt[i]]);
        if (!step(x, c->cnt[i], 1, v ? v - 1 : 0, 0, 0)) return 0;
    }
    if (air) {
        /* an aircraft's sight z: ((z >> 16) - h / 2) >> 5 with h = clamp(altitude + def +0x170,
           0, 255) (0x4825D8..0x48261C), the caller 0x4827B0 raising the altitude to
           (sea + 1) << 16 (0x48280B..0x48281B) and reading the def's byte at 0x4827FB,
           DISASSEMBLED */
        AirK a;
        int64_t y = s32(U_(REC_LR_Y)), floor_y = ((int64_t)c->p.sea + 1) * 65536, h;
        air_k(&c->p.air[ai], &a);
        if (y < floor_y) y = floor_y;
        h = s16(asr(y, 16)) + a.sight_h;
        h = h < 0 ? 0 : h > 255 ? 255 : h;
        STEP(REC_LR_SZ, sight, (uint32_t)(uint64_t)asr(asr(s32(U_(REC_LR_Z)), 16) - (h >> 1), 5), 0, 0);
    }
    return 1;
}

/* After evaluating candidate s at gt, with U its image: the state advances, and the slot rests
   when it is a fixed point.

   THE FIXED POINT. If the history is full (age 3), U == L1 == L2, L3 equals U's lanes, and the
   unit either has no velocity or a move stamp older than gt - 1, then at gt + 1 every input of
   every candidate -- L1, L2, L3, age, and U for the lanes before each -- equals its value at gt.
   The only dependence on gt is the move stamp's gt - 1 when it moved, which the last clause
   makes inexact at gt and at every later gt; the claim stamp does not move when U == L1. So
   every candidate keeps its value, the winners chosen at gt (the first exact, candidate 0 or 1
   since L1 == U) predict U again, every residual is 0, and the state after evaluating equals the
   state before: skipping the slot is identical to evaluating it. A resting slot whose image
   changes has a nonzero residual at its first changed lane -- every lane before it unchanged, so
   its prediction is the old value -- and is listed, which evaluates it. */
static void slot_advance(RecCodec* c, unsigned s, const unsigned char* u, uint32_t gt)
{
    unsigned char* l1 = (unsigned char*)(c->unit + (size_t)s * NDW);
    unsigned char* l2 = (unsigned char*)(c->l2 + (size_t)s * NDW);
    unsigned char* l3 = c->l3 + (size_t)s * L3B;
    unsigned char ul3[L3B];
    int rest = 0;
    if (c->age[s] == 3 && !memcmp(u, l1, IMG) && !memcmp(l1, l2, IMG)) {
        l3_of(c, u, ul3);
        if (!memcmp(l3, ul3, L3B)) {
            int still = !lane_get(u, &c->p.lane[c->lane_of[REC_LR_VX]]) &&
                        !lane_get(u, &c->p.lane[c->lane_of[REC_LR_VY]]) &&
                        !lane_get(u, &c->p.lane[c->lane_of[REC_LR_VZ]]);
            rest = still || (uint64_t)lane_get(u, &c->p.lane[c->lane_of[REC_LR_STAMP]]) + 1 < gt;
        }
    }
    c->rest[s] = (unsigned char)rest;
    l3_of(c, l2, l3);
    memcpy(l2, l1, IMG);
    memcpy(l1, u, IMG);
    if (c->age[s] < 3) c->age[s]++;
}

/* an RK_UNIT or RK_UNIT_GONE applied to the copy, `b` its body (slot and length checked) */
static void unit_record(RecCodec* c, unsigned kind, const unsigned char* b)
{
    unsigned s = get16(b);
    if (kind == RK_UNIT) {
        memcpy(c->unit + (size_t)s * NDW, b + 4, IMG);
        c->live[s] = 1;
    } else {
        c->live[s] = 0;
    }
}

static void touch(RecCodec* c, unsigned s)
{
    if (!c->touched[s]) {
        c->touched[s] = 1;
        rb_u32(&c->tl, s);
    }
}

/* the end of an iteration for the slots its records named */
static void touched_end(RecCodec* c)
{
    size_t i;
    const uint32_t* t = (const uint32_t*)c->tl.p;
    for (i = 0; i < c->tl.n / 4; i++) {
        slot_fresh(c, t[i]);
        c->touched[t[i]] = 0;
    }
    c->tl.n = 0;
}

static int bit_of(const unsigned char* bits, unsigned i) { return bits[i >> 3] >> (i & 7) & 1; }

/* ---- the encoder ---------------------------------------------------------------------------- */

/* The copy takes records [at, n) by the writer's own rules (tagpu_recwriter.c `apply`): every
   iteration's RK_UNIT and RK_UNIT_GONE, and on a decline the deltas the frame did not carry, so
   the copy always ends as the payload leaves the units. A delta the frame did carry is U already,
   which applying it again leaves. */
static void plain_apply(RecCodec* c, const unsigned char* pl, size_t at, size_t n)
{
    while (n - at >= 4) {
        unsigned kind = get16(pl + at), len = get16(pl + at + 2), s, i, k;
        size_t rl = ((size_t)4 + len + 3) & ~(size_t)3;
        const unsigned char* b = pl + at + 4;
        if (rl > n - at) break;
        s = len >= 2 ? get16(b) : 0;
        if (s >= 1 && s < c->nslots) {
            if (kind == RK_UNIT && len == 4 + IMG) {
                unit_record(c, kind, b);
            } else if (kind == RK_UNIT_GONE && len == 4) {
                unit_record(c, kind, b);
            } else if (kind == RK_UNIT_DELTA && len >= REC_UNIT_DELTA_HEAD && c->live[s]) {
                unsigned nb = 0, ok = 1;
                for (i = 0; i < REC_UNIT_BITS * 8; i++)
                    if (bit_of(b + 2, i)) {
                        if (i >= NDW) ok = 0;
                        nb++;
                    }
                if (ok && len == REC_UNIT_DELTA_HEAD + 4 * nb)
                    for (i = 0, k = 0; i < NDW; i++)
                        if (bit_of(b + 2, i))
                            memcpy(c->unit + (size_t)s * NDW + i, b + REC_UNIT_DELTA_HEAD + 4 * k++, 4);
            }
        }
        at += rl;
    }
}

/* Every record checked and routed: the kinds as runs, the bodies into their sections. 0: the
   frame does not carry this payload. */
static int enc_prepass(RecCodec* c, const unsigned char* pl, size_t n, unsigned* ticks)
{
    size_t at = 0;
    unsigned i, last = 0, run = 0, nt = 0;
    for (i = 0; i < RCS_COUNT; i++) c->sec[i].n = 0, c->sec[i].oom = 0;
    if (n < 4 || n % 4 || n > RECC_PAYLOAD_MAX) return 0;
    while (at < n) {
        unsigned kind = get16(pl + at), len = get16(pl + at + 2);
        size_t rl = ((size_t)4 + len + 3) & ~(size_t)3, k;
        const unsigned char* b = pl + at + 4;
        if (!kind || rl > n - at) return 0;
        for (k = 4 + len; k < rl; k++)
            if (pl[at + k]) return 0;
        if (!at && kind != RK_TICK) return 0;
        if (kind == RK_TICK && len != 4) return 0;
        if ((kind == RK_UNIT && len != 4 + IMG) || (kind == RK_UNIT_GONE && len != 4)) return 0;
        if ((kind == RK_UNIT || kind == RK_UNIT_GONE) && (get16(b) < 1 || get16(b) >= c->nslots)) return 0;
        if (kind == RK_UNIT_DELTA && len < REC_UNIT_DELTA_HEAD) return 0;
        nt += kind == RK_TICK;
        if (kind != RK_UNIT_DELTA) {
            RecBytes* s = &c->sec[rec_section_of(kind)];
            rb_varint(s, len);
            rb_put(s, b, len);
        }
        if (kind != last) {
            if (run) rb_varint(&c->sec[RCS_KINDS], run);
            rb_varint(&c->sec[RCS_KINDS], kind);
            last = kind;
            run = 0;
        }
        run++;
        at += rl;
    }
    rb_varint(&c->sec[RCS_KINDS], run);
    for (i = 0; i < RCS_COUNT; i++)
        if (c->sec[i].oom) return 0;
    *ticks = nt;
    return nt >= 1 && nt <= RECC_TICKS_MAX;
}

/* one iteration [at, end) of the payload; 0: declined, nothing of it applied yet */
static int enc_iteration(RecCodec* c, const unsigned char* pl, size_t at, size_t end, unsigned it)
{
    size_t p, nd, k;
    uint32_t gt = get32(pl + at + 4);
    unsigned prev = 0, s, i;
    Eval x;
    c->tl.n = 0;
    c->dl.n = 0;
    for (p = at; p < end; p = p + ((4 + (size_t)get16(pl + p + 2) + 3) & ~(size_t)3)) {
        unsigned kind = get16(pl + p);
        if (kind == RK_UNIT || kind == RK_UNIT_GONE) touch(c, get16(pl + p + 4));
        else if (kind == RK_UNIT_DELTA) rb_u32(&c->dl, (uint32_t)p);
    }
    if (c->tl.oom || c->dl.oom) goto decline;
    nd = c->dl.n / 4;
    /* a delta the frame makes: on a candidate, slots ascending, its bitmap exactly the dwords
       that differ from the copy, in the writer's own layout */
    for (k = 0; k < nd; k++) {
        const unsigned char* b = pl + ((const uint32_t*)c->dl.p)[k] + 4;
        unsigned len = get16(b - 2), nb = 0;
        const unsigned char* old;
        s = get16(b);
        if (s <= prev || s >= c->nslots || !c->live[s] || c->touched[s]) goto decline;
        if (b[13] || b[14] || b[15]) goto decline;
        old = (const unsigned char*)(c->unit + (size_t)s * NDW);
        for (i = 0; i < REC_UNIT_BITS * 8; i++)
            if (bit_of(b + 2, i)) {
                if (i >= NDW || len < REC_UNIT_DELTA_HEAD + 4 * (nb + 1) ||
                    !memcmp(old + 4 * i, b + REC_UNIT_DELTA_HEAD + 4 * nb, 4))
                    goto decline;
                nb++;
            }
        if (!nb || len != REC_UNIT_DELTA_HEAD + 4 * nb) goto decline;
        prev = s;
    }
    memset(&x, 0, sizeof x);
    x.c = c;
    x.gt = gt;
    k = 0;
    c->lcount[it] = 0;
    for (s = 1; s < c->nslots; s++) {
        const unsigned char* d = NULL;
        unsigned j, nb;
        if (!c->live[s] || c->touched[s]) continue;
        if (k < nd && get16(pl + ((const uint32_t*)c->dl.p)[k] + 4) == s)
            d = pl + ((const uint32_t*)c->dl.p)[k++] + 4;
        if (!d && c->rest[s]) continue;
        x.l1 = (const unsigned char*)(c->unit + (size_t)s * NDW);
        x.l2 = (const unsigned char*)(c->l2 + (size_t)s * NDW);
        x.l3 = c->l3 + (size_t)s * L3B;
        x.win = c->win + (size_t)s * RECC_LANES_MAX;
        x.age = c->age[s];
        memcpy(x.u, x.l1, IMG);
        if (d)
            for (i = 0, nb = 0; i < NDW; i++)
                if (bit_of(d + 2, i)) memcpy(x.u + 4 * i, d + REC_UNIT_DELTA_HEAD + 4 * nb++, 4);
        memset(x.rmask, 0, sizeof x.rmask);
        x.any = 0;
        frame_slot(&x);
        if (x.any && !c->over) {
            unsigned mb = (c->p.nlanes + 7) / 8;
            rb_u32(&c->lslot, s);
            rb_put(&c->lmask, x.rmask, mb);
            for (j = 0; j < c->p.nlanes; j++)
                if (bit_of(x.rmask, j)) rb_u32(&c->lzz, x.zz[j]);
            c->lcount[it]++;
            if (c->lslot.n + c->lmask.n + c->lzz.n > RECC_FRAME_CAP) c->over = 1;
        }
        slot_advance(c, s, x.u, gt);
    }
    plain_apply(c, pl, at, end);
    touched_end(c);
    return 1;
decline:
    for (k = 0; k < c->tl.n / 4; k++) c->touched[((const uint32_t*)c->tl.p)[k]] = 0;
    c->tl.n = 0;
    return 0;
}

static uint32_t fnv(const unsigned char* p, unsigned n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

/* RCS_UNITS from the listed units (tagpu_recfmt.h): 0 when it passes RECC_FRAME_CAP or memory */
static int units_write(RecCodec* c, unsigned iterations)
{
    RecBytes* o = &c->sec[RCS_UNITS];
    unsigned mb = (c->p.nlanes + 7) / 8, it, j;
    size_t total = c->lslot.n / 4, i, li, hcap = 16, ndict = 0, z;
    const uint32_t* sl = (const uint32_t*)c->lslot.p;
    uint32_t *hash, *ids, *colv;
    size_t colat[RECC_LANES_MAX], coln[RECC_LANES_MAX];
    o->n = 0;
    o->oom = 0;
    rb_varint(o, iterations);
    for (it = 0; it < iterations; it++) rb_varint(o, c->lcount[it]);
    for (it = 0, li = 0; it < iterations; it++) {
        uint32_t prev = 0;
        for (i = 0; i < c->lcount[it]; i++, li++) {
            rb_varint(o, sl[li] - prev - 1);
            prev = sl[li];
        }
    }
    /* the masks, each once, in first-use order: an open-addressing table of twice the listed */
    while (hcap < 2 * total) hcap *= 2;
    c->hash.n = 0;
    c->ids.n = 0;
    if (!rb_grow(&c->hash, hcap * 4) || !rb_grow(&c->ids, total * 4 + 4)) return 0;
    hash = (uint32_t*)c->hash.p;
    ids = (uint32_t*)c->ids.p;
    memset(hash, 0, hcap * 4);
    for (i = 0; i < total; i++) {
        const unsigned char* m = c->lmask.p + i * mb;
        size_t h = fnv(m, mb) & (hcap - 1);
        while (hash[h] && memcmp(c->lmask.p + (size_t)ids[hash[h] - 1] * mb, m, mb))
            h = (h + 1) & (hcap - 1);
        if (!hash[h]) {
            hash[h] = (uint32_t)i + 1;         /* the listed unit whose mask it is, plus one */
            ids[i] = (uint32_t)i;
            ndict++;
        } else {
            ids[i] = ids[hash[h] - 1];
        }
    }
    /* ids hold the first user of each mask; number the masks in first-use order */
    rb_varint(o, (uint32_t)ndict);
    {
        size_t next = 0;
        for (i = 0; i < total; i++) {
            if (ids[i] == i) {
                rb_put(o, c->lmask.p + i * mb, mb);
                ids[i] = (uint32_t)(next++ | 0x80000000u);
            } else {
                ids[i] = ids[ids[i]];
            }
        }
        for (i = 0; i < total; i++) {
            ids[i] &= 0x7FFFFFFFu;
            if (ndict <= 256) {
                unsigned char b = (unsigned char)ids[i];
                rb_put(o, &b, 1);
            } else {
                rb_varint(o, ids[i]);
            }
        }
    }
    /* the columns: each lane's residuals in listed order */
    memset(coln, 0, sizeof coln);
    for (i = 0; i < total; i++)
        for (j = 0; j < c->p.nlanes; j++) coln[j] += bit_of(c->lmask.p + i * mb, j);
    for (j = 0, z = 0; j < c->p.nlanes; j++) {
        colat[j] = z;
        z += coln[j];
    }
    c->cols.n = 0;
    if (!rb_grow(&c->cols, z * 4 + 4)) return 0;
    colv = (uint32_t*)c->cols.p;
    for (i = 0, li = 0; i < total; i++)
        for (j = 0; j < c->p.nlanes; j++)
            if (bit_of(c->lmask.p + i * mb, j)) colv[colat[j]++] = ((const uint32_t*)c->lzz.p)[li++];
    for (i = 0; i < z; i++) rb_varint(o, colv[i]);
    return !o->oom && o->n <= RECC_FRAME_CAP;
}

int reccodec_encode(RecCodec* e, const unsigned char* pl, size_t n, RecBytes* out)
{
    unsigned ticks = 0, it = 0, i;
    size_t at = 0;
    RecSecHead h;
    if (!e->valid || !enc_prepass(e, pl, n, &ticks)) {
        plain_apply(e, pl, 0, n);
        e->valid = 0;
        return RECC_DECLINED;
    }
    e->lslot.n = e->lmask.n = e->lzz.n = 0;
    e->lslot.oom = e->lmask.oom = e->lzz.oom = 0;
    e->over = 0;
    while (at < n) {
        size_t end = at + 8;            /* the iteration's RK_TICK is 8 bytes */
        while (end < n && get16(pl + end) != RK_TICK) end += ((4 + (size_t)get16(pl + end + 2) + 3) & ~(size_t)3);
        if (!enc_iteration(e, pl, at, end, it)) {
            plain_apply(e, pl, at, n);
            e->valid = 0;
            return RECC_DECLINED;
        }
        at = end;
        it++;
    }
    if (e->over || e->lslot.oom || e->lmask.oom || e->lzz.oom || !units_write(e, ticks)) {
        e->valid = 0;
        return RECC_DECLINED;
    }
    memset(&h, 0, sizeof h);
    h.payload = (uint32_t)n;
    h.iterations = (uint16_t)ticks;
    h.sections = RCS_COUNT;
    for (i = 0; i < RCS_COUNT; i++) h.len[i] = (uint32_t)e->sec[i].n;
    out->n = 0;
    out->oom = 0;
    rb_put(out, &h, sizeof h);
    for (i = 0; i < RCS_COUNT; i++) rb_put(out, e->sec[i].p, e->sec[i].n);
    if (out->oom) {
        e->valid = 0;
        return RECC_DECLINED;
    }
    return RECC_SECTIONS;
}

/* ---- the decoder ---------------------------------------------------------------------------- */

typedef struct Cursor {             /* a walk over the kinds' runs and the body sections */
    size_t run, left;               /* the run, and its records not yet walked            */
    size_t at[RCS_COUNT];
} Cursor;

int reccodec_decode(RecCodec* d, const unsigned char* sec, size_t n, RecBytes* out, char* why,
                    unsigned cap)
{
    RecSecHead h;
    size_t off[RCS_COUNT + 1], at, total = 0, records = 0, nruns, li, i;
    size_t colat[RECC_LANES_MAX], coln[RECC_LANES_MAX];
    unsigned it, ticks = 0, mb, j, ndict = 0;
    uint32_t v, kind;
    uint32_t listed[RECC_TICKS_MAX];
    const unsigned char* dict;
    Cursor cur;
    if (!d->valid) REFUSE("a sectioned block with no unit frame to continue");
    d->valid = 0;
    d->tl.n = 0;
    if (n < sizeof h) REFUSE("a sectioned block of %lu bytes, under its 36-byte head", (unsigned long)n);
    h.payload = get32(sec);
    h.iterations = (uint16_t)get16(sec + 4);
    h.sections = (uint16_t)get16(sec + 6);
    if (h.sections != RCS_COUNT) REFUSE("a sectioned block of %u sections, not %u", h.sections, RCS_COUNT);
    off[0] = sizeof h;
    for (i = 0; i < RCS_COUNT; i++) {
        h.len[i] = get32(sec + 8 + 4 * i);
        if (h.len[i] > n - off[i]) REFUSE("section %lu's %lu bytes run past the block's %lu",
                                          (unsigned long)i, (unsigned long)h.len[i], (unsigned long)n);
        off[i + 1] = off[i] + h.len[i];
    }
    if (off[RCS_COUNT] != n) REFUSE("the sections end at byte %lu of %lu", (unsigned long)off[RCS_COUNT], (unsigned long)n);
    if (h.payload < 4 || h.payload % 4 || h.payload > RECC_PAYLOAD_MAX)
        REFUSE("a payload of %lu bytes: not a multiple of 4 in 4..%u", (unsigned long)h.payload, RECC_PAYLOAD_MAX);
    if (h.iterations < 1 || h.iterations > RECC_TICKS_MAX)
        REFUSE("%u iterations, not 1..%u", h.iterations, RECC_TICKS_MAX);

    /* RCS_KINDS */
    d->runs.n = 0;
    d->runs.oom = 0;
    for (at = off[RCS_KINDS]; at < off[RCS_KINDS + 1];) {
        uint32_t count;
        if (!varint_get(sec, off[RCS_KINDS + 1], &at, &kind) || !varint_get(sec, off[RCS_KINDS + 1], &at, &count))
            REFUSE("a kind run cut short or past 2^32 at byte %lu", (unsigned long)at);
        if (kind < 1 || kind > 0xFFFF) REFUSE("record kind %lu at byte %lu", (unsigned long)kind, (unsigned long)at);
        if (count < 1) REFUSE("a run of no records at byte %lu", (unsigned long)at);
        if (!records && kind != RK_TICK) REFUSE("the first record is kind %lu, not RK_TICK", (unsigned long)kind);
        if (count > h.payload / 4 - records) REFUSE("more records than a %lu-byte payload holds", (unsigned long)h.payload);
        records += count;
        if (kind == RK_TICK) ticks += count;    /* records <= 2^24: no overflow */
        rb_u32(&d->runs, kind);
        rb_u32(&d->runs, count);
    }
    if (!records) REFUSE("no records");
    if (ticks != h.iterations) REFUSE("%u RK_TICK records, not the head's %u iterations", ticks, h.iterations);
    if (d->runs.oom) REFUSE("no memory for the kinds");
    nruns = d->runs.n / 8;

    /* RCS_UNITS: everything read and bounded before the first iteration */
    at = off[RCS_UNITS];
    mb = (d->p.nlanes + 7) / 8;
    if (!varint_get(sec, off[RCS_UNITS + 1], &at, &v) || v != h.iterations)
        REFUSE("the unit frame's iterations are not the head's %u", h.iterations);
    for (it = 0; it < h.iterations; it++) {
        if (!varint_get(sec, off[RCS_UNITS + 1], &at, &listed[it]) || listed[it] > d->nslots)
            REFUSE("iteration %u lists past the %u slots, at byte %lu", it, d->nslots, (unsigned long)at);
        total += listed[it];
    }
    d->lslot.n = 0;
    d->lslot.oom = 0;
    for (it = 0; it < h.iterations; it++) {
        uint64_t slot = 0;
        for (i = 0; i < listed[it]; i++) {
            if (!varint_get(sec, off[RCS_UNITS + 1], &at, &v)) REFUSE("a slot gap cut short at byte %lu", (unsigned long)at);
            slot += (uint64_t)v + 1;
            if (slot >= d->nslots) REFUSE("listed slot %llu is past the %u slots", (unsigned long long)slot, d->nslots);
            rb_u32(&d->lslot, (uint32_t)slot);
        }
    }
    if (!varint_get(sec, off[RCS_UNITS + 1], &at, &v)) REFUSE("the mask count cut short at byte %lu", (unsigned long)at);
    if (total ? (v < 1 || v > total) : v != 0) REFUSE("%lu masks for %lu listed units", (unsigned long)v, (unsigned long)total);
    ndict = v;
    if ((size_t)ndict * mb > off[RCS_UNITS + 1] - at) REFUSE("the masks run past the unit frame");
    dict = sec + at;
    for (i = 0; i < ndict; i++) {
        const unsigned char* m = dict + i * mb;
        unsigned any = 0;
        for (j = 0; j < mb; j++) any |= m[j];
        if (!any) REFUSE("mask %lu is empty", (unsigned long)i);
        if (d->p.nlanes % 8 && m[mb - 1] >> (d->p.nlanes % 8)) REFUSE("mask %lu names a lane past %u", (unsigned long)i, d->p.nlanes);
    }
    at += (size_t)ndict * mb;
    d->ids.n = 0;
    d->ids.oom = 0;
    for (i = 0; i < total; i++) {
        if (ndict <= 256) {
            if (at >= off[RCS_UNITS + 1]) REFUSE("the mask ids cut short");
            v = sec[at++];
        } else if (!varint_get(sec, off[RCS_UNITS + 1], &at, &v)) {
            REFUSE("a mask id cut short at byte %lu", (unsigned long)at);
        }
        if (v >= ndict) REFUSE("mask id %lu past the %u masks", (unsigned long)v, ndict);
        rb_u32(&d->ids, v);
    }
    if (d->lslot.oom || d->ids.oom) REFUSE("no memory for the listed units");
    memset(coln, 0, sizeof coln);
    for (i = 0; i < total; i++)
        for (j = 0; j < d->p.nlanes; j++) coln[j] += bit_of(dict + (size_t)((const uint32_t*)d->ids.p)[i] * mb, j);
    for (j = 0; j < d->p.nlanes; j++) {
        unsigned w = d->p.lane[j].width;
        colat[j] = at;
        for (i = 0; i < coln[j]; i++) {
            if (!varint_get(sec, off[RCS_UNITS + 1], &at, &v)) REFUSE("lane %u's column cut short at byte %lu", j, (unsigned long)at);
            if (w < 4 && v >> (8 * w)) REFUSE("lane %u's residual %lu is past its %u bytes", j, (unsigned long)v, w);
        }
    }
    if (at != off[RCS_UNITS + 1]) REFUSE("%lu bytes left in the unit frame", (unsigned long)(off[RCS_UNITS + 1] - at));

    out->n = 0;
    out->oom = 0;
    if (!rb_grow(out, h.payload)) REFUSE("no memory for a %lu-byte payload", (unsigned long)h.payload);

    /* the iterations */
    memset(&cur, 0, sizeof cur);
    for (i = 0; i < RCS_COUNT; i++) cur.at[i] = off[i];
    cur.left = ((const uint32_t*)d->runs.p)[1];
    li = 0;
    for (it = 0; it < h.iterations; it++) {
        Cursor a = cur;
        unsigned ndelta = 0, nchanged = 0, s, first = 1;
        uint32_t gt = 0;
        size_t li1 = li + listed[it];
        Eval x;
        /* pass A: the iteration's records, with copies of the cursors */
        d->dl.n = 0;
        d->dl.oom = 0;
        for (;;) {
            uint32_t len;
            unsigned sc;
            const unsigned char* b;
            if (!a.left) {
                if (++a.run >= nruns) break;
                a.left = ((const uint32_t*)d->runs.p)[2 * a.run + 1];
            }
            kind = ((const uint32_t*)d->runs.p)[2 * a.run];
            if (kind == RK_TICK && !first) break;
            first = 0;
            a.left--;
            if (kind == RK_UNIT_DELTA) {
                ndelta++;
                continue;
            }
            sc = rec_section_of(kind);
            if (!varint_get(sec, off[sc + 1], &a.at[sc], &len) || len > 0xFFFF || len > off[sc + 1] - a.at[sc])
                REFUSE("a body of kind %lu cut short or past 0xFFFF in section %u at byte %lu",
                       (unsigned long)kind, sc, (unsigned long)a.at[sc]);
            b = sec + a.at[sc];
            a.at[sc] += len;
            if (kind == RK_TICK) {
                if (len != 4) REFUSE("an RK_TICK of %lu bytes", (unsigned long)len);
                gt = get32(b);
            } else if (kind == RK_UNIT || kind == RK_UNIT_GONE) {
                if (len != (kind == RK_UNIT ? 4u + IMG : 4u)) REFUSE("a unit record of kind %lu of %lu bytes", (unsigned long)kind, (unsigned long)len);
                if (get16(b) < 1 || get16(b) >= d->nslots) REFUSE("a unit record for slot %u, outside 1..%u", get16(b), d->nslots - 1);
                touch(d, get16(b));
            }
        }
        if (d->tl.oom) REFUSE("no memory for the touched slots");
        /* the frame: the candidates in ascending order, each listed one among them */
        memset(&x, 0, sizeof x);
        x.c = d;
        x.gt = gt;
        x.dec = 1;
        x.sec = sec;
        x.secend = off[RCS_UNITS + 1];
        x.colat = colat;
        for (s = 1; s < d->nslots; s++) {
            int cand = d->live[s] && !d->touched[s];
            int lst = li < li1 && ((const uint32_t*)d->lslot.p)[li] == s;
            unsigned char* l1;
            if (lst && !cand) REFUSE("iteration %u lists slot %u, which is not a candidate", it, s);
            if (!cand || (d->rest[s] && !lst)) continue;
            l1 = (unsigned char*)(d->unit + (size_t)s * NDW);
            x.l1 = l1;
            x.l2 = (const unsigned char*)(d->l2 + (size_t)s * NDW);
            x.l3 = d->l3 + (size_t)s * L3B;
            x.win = d->win + (size_t)s * RECC_LANES_MAX;
            x.age = d->age[s];
            x.mask = lst ? dict + (size_t)((const uint32_t*)d->ids.p)[li] * mb : NULL;
            li += lst;
            memset(x.u, 0, IMG);
            if (!frame_slot(&x)) REFUSE("a residual cut short");
            if (memcmp(x.u, l1, IMG)) {
                unsigned char head[REC_UNIT_DELTA_HEAD];
                unsigned q, nb = 0;
                memset(head, 0, sizeof head);
                head[0] = (unsigned char)s;
                head[1] = (unsigned char)(s >> 8);
                for (q = 0; q < NDW; q++)
                    if (memcmp(x.u + 4 * q, l1 + 4 * q, 4)) {
                        head[2 + (q >> 3)] |= (unsigned char)(1u << (q & 7));
                        nb++;
                    }
                rb_u32(&d->dl, REC_UNIT_DELTA_HEAD + 4 * nb);
                rb_put(&d->dl, head, sizeof head);
                for (q = 0; q < NDW; q++)
                    if (memcmp(x.u + 4 * q, l1 + 4 * q, 4)) rb_put(&d->dl, x.u + 4 * q, 4);
                nchanged++;
            }
            slot_advance(d, s, x.u, gt);
        }
        if (d->dl.oom) REFUSE("no memory for the deltas");
        if (nchanged != ndelta) REFUSE("iteration %u: the frame changes %u units, its kinds hold %u deltas", it, nchanged, ndelta);
        /* pass B: the records, in order */
        {
            size_t dat = 0;
            first = 1;
            for (;;) {
                uint32_t len;
                unsigned sc;
                unsigned char hd[4];
                const unsigned char* b;
                size_t rl;
                if (!cur.left) {
                    if (++cur.run >= nruns) break;
                    cur.left = ((const uint32_t*)d->runs.p)[2 * cur.run + 1];
                }
                kind = ((const uint32_t*)d->runs.p)[2 * cur.run];
                if (kind == RK_TICK && !first) break;
                first = 0;
                cur.left--;
                if (kind == RK_UNIT_DELTA) {
                    memcpy(&len, d->dl.p + dat, 4);
                    b = d->dl.p + dat + 4;
                    dat += 4 + len;
                } else {
                    sc = rec_section_of(kind);
                    varint_get(sec, off[sc + 1], &cur.at[sc], &len);    /* read in pass A */
                    b = sec + cur.at[sc];
                    cur.at[sc] += len;
                    if (kind == RK_UNIT || kind == RK_UNIT_GONE) unit_record(d, kind, b);
                }
                rl = ((size_t)4 + len + 3) & ~(size_t)3;
                if (rl > h.payload - out->n) REFUSE("the records pass the head's %lu-byte payload", (unsigned long)h.payload);
                hd[0] = (unsigned char)kind;
                hd[1] = (unsigned char)(kind >> 8);
                hd[2] = (unsigned char)len;
                hd[3] = (unsigned char)(len >> 8);
                rb_put(out, hd, 4);
                rb_put(out, b, len);
                memset(out->p + out->n, 0, rl - 4 - len);
                out->n += rl - 4 - len;
            }
        }
        touched_end(d);
    }
    for (i = RCS_PROJ; i < RCS_COUNT; i++)
        if (cur.at[i] != off[i + 1]) REFUSE("%lu bytes left in section %lu", (unsigned long)(off[i + 1] - cur.at[i]), (unsigned long)i);
    if (out->n != h.payload) REFUSE("the records are %lu bytes, not the head's %lu", (unsigned long)out->n, (unsigned long)h.payload);
    d->valid = 1;
    return 1;
}

/* ---- zstd ----------------------------------------------------------------------------------- */

int reccodec_pack(void* cctx, RecBytes* out, const unsigned char* src, size_t n)
{
    size_t z;
    out->n = 0;
    out->oom = 0;
    if (!n || !rb_grow(out, n)) return 0;
    z = ZSTD_compressCCtx((ZSTD_CCtx*)cctx, out->p, n - 1 ? n - 1 : 1, src, n, 3);
    if (ZSTD_isError(z) || z >= n) return 0;
    out->n = z;
    return 1;
}

/* The largest window a frame may ask for, 2^27: zstd's streaming default
   (ZSTD_WINDOWLOG_LIMIT_DEFAULT), which the Python reader's decompressor enforces and the one-shot
   ZSTD_decompressDCtx does not. Bounding it here by the frame's header makes both readers refuse
   the same frames. The writer's frames ask for far less: level 3's window is 2^21
   (ZSTD_defaultCParameters), or the block's own size when that is smaller. */
#define RECC_WINDOW_MAX (1ull << 27)

int reccodec_unpack(void* dctx, unsigned char* dst, size_t raw, const unsigned char* src,
                    size_t packed)
{
    unsigned long long size;
    ZSTD_FrameHeader fh;
    if (!raw || !packed) return 0;
    if (ZSTD_findFrameCompressedSize(src, packed) != packed) return 0;
    size = ZSTD_getFrameContentSize(src, packed);
    if (size != (unsigned long long)raw) return 0;
    if (ZSTD_getFrameHeader(&fh, src, packed) != 0 || fh.frameType != ZSTD_frame ||
        fh.windowSize > RECC_WINDOW_MAX)
        return 0;
    return ZSTD_decompressDCtx((ZSTD_DCtx*)dctx, dst, raw, src, packed) == raw;
}

/* ---- the writer's codec --------------------------------------------------------------------- */

int reccodec_stream_begin(RecStream* s, const RecCodecParams* p)
{
    memset(s, 0, sizeof *s);
    s->p = *p;
    s->enc = reccodec_new(p->nslots);
    s->chk = reccodec_new(p->nslots);
    s->cctx = ZSTD_createCCtx();
    s->dctx = ZSTD_createDCtx();
    s->reset = 1;
    if (!s->enc || !s->chk || !s->cctx || !s->dctx || !p->usable) {
        reccodec_stream_end(s);
        return 0;
    }
    return 1;
}

void reccodec_stream_end(RecStream* s)
{
    reccodec_free(s->enc);
    reccodec_free(s->chk);
    ZSTD_freeCCtx((ZSTD_CCtx*)s->cctx);
    ZSTD_freeDCtx((ZSTD_DCtx*)s->dctx);
    rb_free(&s->sec);
    rb_free(&s->pk);
    rb_free(&s->back);
    rb_free(&s->out);
    s->enc = s->chk = NULL;
    s->cctx = s->dctx = NULL;
}

/* DECODE BEFORE WRITE: the block is written sectioned only when the check decoder D, from the
   state a reader will have, gives back these records from the very bytes written. D is a second
   state, not the encoder's: the check has to start where a reader starts and must not share the
   encoder's arithmetic. A records block, and every snapshot (the writer's), make the next block
   restart both from the encoder's copy, which is always the payload's end state. */
int reccodec_stream_block(RecStream* s, const unsigned char* payload, size_t n, unsigned* flags,
                          const unsigned char** body, size_t* bn)
{
    int k, reset = s->reset, packed;
    if (reset) {
        reccodec_restart(s->enc, &s->p, reccodec_units(s->enc), reccodec_live(s->enc));
        reccodec_restart(s->chk, &s->p, reccodec_units(s->enc), reccodec_live(s->enc));
    }
    k = reccodec_encode(s->enc, payload, n, &s->sec);
    if (k == RECC_SECTIONS) {
        const unsigned char* src = s->sec.p;
        packed = reccodec_pack(s->cctx, &s->pk, s->sec.p, s->sec.n);
        if (packed) {
            s->back.n = 0;
            s->back.oom = 0;
            if (!rb_grow(&s->back, s->sec.n) ||
                !reccodec_unpack(s->dctx, s->back.p, s->sec.n, s->pk.p, s->pk.n))
                k = RECC_MISMATCH;
            src = s->back.p;
        }
        if (k == RECC_SECTIONS &&
            (!reccodec_decode(s->chk, src, s->sec.n, &s->out, NULL, 0) || s->out.n != n ||
             memcmp(s->out.p, payload, n)))
            k = RECC_MISMATCH;
        if (k == RECC_SECTIONS) {
            *flags = RBF_SECTIONS | (reset ? RBF_FRAME_RESET : 0) | (packed ? 0 : RBF_STORED);
            *body = packed ? s->pk.p : s->sec.p;
            *bn = packed ? s->pk.n : s->sec.n;
            s->reset = 0;
            s->sectioned++;
            return k;
        }
    }
    if (k == RECC_MISMATCH) s->mismatched++;
    else s->declined++;
    reccodec_invalidate(s->enc);
    reccodec_invalidate(s->chk);
    s->reset = 1;
    packed = reccodec_pack(s->cctx, &s->pk, payload, n);
    *flags = packed ? 0 : RBF_STORED;
    *body = packed ? s->pk.p : payload;
    *bn = packed ? s->pk.n : n;
    return k;
}
