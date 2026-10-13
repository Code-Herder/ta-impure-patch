/* tagpu_reccodec.c -- the demo recorder's stream codec.

   A stream block may be a SECTIONED CONTAINER (tagpu_recfmt.h, RecSecHead and RCS_*): the
   record kinds as patterns, the UNIT FRAME, which carries every RK_UNIT_DELTA as residuals against a
   prediction, the POOL FRAME, which carries every RK_PROJ_HEAD, RK_PROJ and RK_PROJ_DELTA, the
   four RECORD FRAMES, which make the bodies of the cells, the orders, the events and the state
   (tagpu_recframe.h; a file each), and the other kinds' bodies verbatim (RCS_OTHER). It decodes to
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

   THE POOL FRAME. RCS_PROJ carries the projectile pool coded by identity. The compaction keeps
   the survivors in order at the front and appends at the count (exe-reverse-engineering.md, the
   projectile pool), so an iteration is: the old records that died, the survivors (each predicted
   from its own last image by the engine's rules, pool_step), the births (each against a reference
   row: the new pool so far, the old pool, the old pool advanced a step, or a row made from the
   last iteration's shot events), and the stale slots past the count that changed. The decoder
   rebuilds the pool and makes every record's body from it (pool_iter): the records are the
   changed slots of the pool, so the pool before and after an iteration fix them (pool_check).
   The shot events an iteration's births refer to are the last iteration's, as its records hold
   them -- the encoder's payload, and in the decoder the bodies the events frame made -- so the
   decoder runs the pool frame an iteration at a time, between the record walks.

   THE RECORD FRAMES. The encoder walks each iteration's records in order after the unit and pool
   frames (frames_walk): each frame takes the records of its kinds, and RK_UNIT and RK_UNIT_GONE
   update the unit copy as they come, so a frame sees at each record the copy the decoder's walk
   has there (tagpu_recframe.h, RecFrameAt). The decoder's pass B makes each frame record's body
   from its frame in the same order.

   THE KINDS (tagpu_recfmt.h, RCS_KINDS). The same walk writes each iteration's pattern
   (kinds_write): its runs of kinds, the two markers standing for records no frame codes -- an
   empty feature tick whose two bodies are the run's last explicit empty pair, and an RK_FEATURES
   the state frame predicts -- and no count for each group's first run, which the decoder takes
   from the frames: 1 for RK_TICK, the unit frame's changed units for RK_UNIT_DELTA, the pool
   frame's changed records for RK_PROJ and RK_PROJ_DELTA together. The decoder reads the block's
   patterns before its first iteration (kinds_parse) and fills a group's count when its frame has
   made the iteration (group_fill): the pool frame's before pass A, the unit frame's after it, and
   pass A walks no run whose count is open, it being RK_UNIT_DELTA's, whose records pass A skips.
   The pattern list, the last pair and whether an RK_FEATURES was explicit are state of the run,
   cleared at every reset: the encoder's walk keeps all three; on the decoder, kinds_parse moves
   the pattern list and pass B keeps the last pair and the explicit RK_FEATURES, in the encoder's
   record order.

   THREE INVARIANTS make this lossless whatever the codec gets wrong:
   - DECODE BEFORE WRITE (an ordering). The writer encodes a block with its encoder, packs it as
     its run's next chunk, unpacks those bytes with a decoder that has unpacked the run's chunks
     before them and decodes them with a second state through this file's decoder, and writes the
     sectioned block only when that gives back the records byte for byte; otherwise it writes the
     records (reccodec_stream_block). A codec bug costs size, never data. A snapshot's pack is
     checked the same way, against its prefix (reccodec_snapshot).
   - RESET POINTS (an ordering). The frames' state, and the run's zstd frame with it
     (tagpu_recfmt.h, THE RUN AND THE CHAIN), cross block boundaries only inside a run of
     sectioned blocks. All restart (RBF_FRAME_RESET) at a segment's first stream block, at the
     first after a snapshot and at the first after a records block: the unit and pool frames from
     the copy of the units and of the projectile pool a reader holds there, the cells frame from
     the grid it holds there, and the orders, events and state frames empty. A reader starts only
     at a snapshot or a segment's start and reads a block only between iterations. Every piece of
     pool frame state that crosses a block is rebuilt from that pool or cleared, and every record
     frame's cleared or taken from the world (reccodec_restart); the only outside state a record
     frame reads is the unit copy and that world, which the restart takes from the reader's units
     and grid in the encoder, the check decoder and every reader alike. Every reset hands the
     frames the reader's grid there (RecFrameWorld). The encoder keeps that grid itself through
     every block, coded or declined (the cells frame's world, plain_apply), and the writer
     restarts both its states from it. INVARIANT: the grid at a reset is the same bytes in every
     walk. Each is the images of the segment's records so far over zeros, by one rule (an RK_CELLS
     with no event before it in its iteration), each record bounded by the grid; a seeking reader's
     comes from a snapshot of the writer's copy, built by the same rule by construction
     (reccodec_stream_block's INVARIANT says how), which tarec.py check compares with the stream's.
     So a record frame's state is a function of that world, the records since the reset and the
     unit copy, and a decline or a refusal needs no rollback: the codec
     turns invalid and the next block restarts. A block that continues a frame this decoder did
     not start is refused, never misread. A record frame never declines mid-block: it latches,
     and its enc_write declines the block after its iterations (tagpu_recframe.h).
   - BOUNDS. Every value read from a file is data. Each length, count, slot, varint, lane, role and
     flag is tested against its bound before it is used and a refusal names it; every operand of a
     rule is clamped before it is multiplied, so no int64 product can overflow; the constants a
     header carries (the defs' values, gravity) are clamped where they are used. The pool frame's
     counts, slots, gaps and references are tested against RT_PROJECTILES, the head's
     iterations and the section's bytes before use; its encoder's work is bounded by
     construction (pool_encode), not by the game's behaviour.

   NO FLOATING POINT. The writer thread starts at x87 precision 64 (mingw's TLS callback runs
   `fninit` on every thread it attaches), the engine runs at 53, and the Python reader
   (tools/replay/reccodec.py) must agree bit for bit: every rule is int64 arithmetic with floor
   shifts and named divisions.

   MEMORY: per slot the copy (0x148 bytes), L2 (0x148), L3 (16), the winners (one byte per lane,
   128), five flags and the last mask (16), 821 bytes; per projectile record the pool and its next image, their
   images one iteration before, the winners (27 bytes each), the ages, the advanced image and the
   survivors' maps, 613 bytes; per codec the air index (128 KB), the last birth of each weapon
   (442 KB), the shot events and their rows (590 KB), the search's three indexes over
   max(nproj, 4,096) rows (12 bytes a row each, and a table of 8-byte slots at least twice the
   rows: 393 KB up to 4,096 records), and the scratch of the largest block met so far
   (reccodec_new). At 3,000 records the pool frame is about 3.3 MB a codec. The kinds keep the
   run's RECC_KINDS_MRU patterns, each at most RECC_KINDS_MAX bytes (1 MB at the bound; MEASURED
   2026-10-12 on the heavy four-player hour: a new pattern 17 bytes on average, 1,862 at most),
   and a block's runs, 12 bytes each. The listing keeps the run's RECC_J2_MTF masks with their
   links and hash chains, 22 bytes each and 16 KB of chains: 106 KB. Each record frame's memory
   is its file's to state. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ZSTD_STATIC_LINKING_ONLY       /* ZSTD_getFrameHeader */
#include "zstd.h"
#include "tagpu_reccodec.h"
#include "tagpu_recframe.h"

#define IMG     REC_UNIT_IMAGE
#define NDW     REC_UNIT_DWORDS
#define L3B     16                /* L3: heading u16, layer u8, 0, vx, vy, vz                       */
#define NOROW   0xFFFFu
#define MB_MAX  ((RECC_LANES_MAX + 7) / 8)
#define J2_HASH (2 * RECC_J2_MTF)     /* the listing's hash chains: a load of at most 1/2        */
typedef char rec_j2_fits_int16[RECC_J2_MTF <= 0x7FFF && (J2_HASH & (J2_HASH - 1)) == 0 ? 1 : -1];

#define REFUSE RECF_REFUSE

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
typedef struct AirK { int64_t maxv, brake, acc, bank, pitch; } AirK;

static void air_k(const RecAirRow* r, AirK* a)
{
    a->maxv = r->maxv < 1 ? 1 : r->maxv > (1 << 24) ? (1 << 24) : r->maxv;
    a->brake = r->brake < 0 ? 0 : r->brake > (1 << 24) ? (1 << 24) : r->brake;
    a->acc = r->acc < 0 ? 0 : r->acc > (1 << 24) ? (1 << 24) : r->acc;
    a->bank = clip(r->bank, 1 << 24);
    a->pitch = clip(r->pitch, 1 << 24);
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

#define PDW   REC_PROJ_DWORDS         /* a projectile image's dwords, 27                          */
#define PIMG  REC_PROJ_IMAGE
/* the longest body pool_body writes: a delta of all 27 dwords (116), past a whole record's 112 */
#define POOL_BODY_MAX (REC_PROJ_DELTA_HEAD + PIMG)
#define SHOT  36                      /* an RK_EV_SHOT's body                                     */

/* The encoder's search index over one reference section's rows (pool_encode): per weapon key the
   latest row and each row's predecessor of the same key, and an open-addressing table of
   (weapon, shooter) keys with each row's predecessor of the same pair. */
typedef struct Chains {
    int32_t   headw[RECC_WEAPON_IDS + 1];
    int32_t  *prevw, *prevws, *top;   /* [rows], [rows], [tsize]: -1 for none                 */
    uint32_t *key, *used;             /* [tsize]; the table slots taken, [rows]               */
    unsigned  tsize, tbits, nused;
} Chains;

/* the record frames, in the order the codec makes, restarts and calls them */
#define RECC_FRAMES 4
#define RECC_OWNERS 16          /* the ledger's owner IDs the codec keeps a settle for: a player's
                                   ID is its index + 1, and an ID at or past this one has none */
static const RecFrameOps* const FRAMES[RECC_FRAMES] = { &recframe_cells, &recframe_orders,
                                                        &recframe_events, &recframe_state };

struct RecCodec {
    unsigned nslots;
    unsigned gw, gh, nwrecks;             /* the sizes the record frames were made for          */
    int valid;
    RecCodecParams p;                     /* the restart's, copied: the codec owns what it reads */
    int lane_of[REC_LR_N];                /* the lane of each role but PLAIN and COUNT           */
    unsigned char s1[RECC_LANES_MAX], s2[RECC_LANES_MAX], cnt[RECC_COUNTS_MAX];
    unsigned ns1, ns2, ncnt;
    unsigned char defwin[RECC_LANES_MAX];
    int64_t q;                            /* (gravity << 16) / 0xCCD (0x43D186..0x43D1A4)       */
    uint16_t* airix;                      /* type -> RT_AIR row, NOROW for none                  */
    uint16_t* typix;                      /* type -> RT_TYPES row, NOROW for none                */
    uint32_t* unit;                       /* the copy: L1                                       */
    uint32_t* l2;
    unsigned char *l3, *win, *age, *rest, *live, *touched;
    uint32_t* lstep;                      /* per slot: the ledger's energy and metal step        */
    uint32_t lsettle[RECC_OWNERS];        /* per owner ID: the last settle the ledger lanes saw  */
    unsigned char lsettled[RECC_OWNERS];  /* ... once one was seen since the restart            */
    RecBytes tl, dl, runs, lmask, lzz, ids, cols;
    RecBytes sec[RCS_COUNT];              /* the encoder's sections                             */
    /* the listing (THE LISTING): per slot whether the iteration before listed it and its last
       mask (MB_MAX bytes, zero before any); the run's masks most recent first, a doubly linked
       list over RECC_J2_MTF stored masks with a hash of chains to find one by its bytes. The
       encoder's block: the symbols, each iteration's ranks and woken slots, the new masks and
       the ids; the decoder's iteration: its ranks and woken slots. */
    unsigned char *jprev, *jlast, *jm;
    int16_t *jnx, *jpv, *jhead, *jhnext;
    int jfront, jtail;
    unsigned njm, jnsym;
    int jbad;                             /* an encoder's woken slot was not listed             */
    RecBytes jsym, jrw, jnew, jr, jw;
    int over;                             /* an encoder's frame passed RECC_FRAME_CAP           */
    /* the pool frame (THE POOL FRAME, pool_restart): nproj records of PDW dwords each */
    unsigned nproj, pcount, phw;          /* the pool's count and extent: pcount <= phw <= nproj */
    unsigned ncount, nhw, nchg, nshots, evnext;
    uint32_t *pool, *pool_n;              /* P, and Pn: the image the iteration makes          */
    uint32_t *pool_l2, *pool_l2n;         /* each live record's image one iteration before     */
    unsigned char *pwin, *pwin_n;         /* each live record's winner per dword, 0..2          */
    unsigned char *page, *page_n;         /* 0 past the count, 1 no history yet, 2               */
    uint32_t* lastnew;                    /* per weapon ID, the last birth with that +0x00      */
    unsigned char* shots;                 /* the last iteration's RK_EV_SHOT bodies             */
    uint32_t* ev;                         /* their rows, PS_EV                                  */
    uint32_t* adv;                        /* PS_ADV's rows, made on demand ...                  */
    unsigned char* adv_done;              /* ... once an iteration                              */
    int32_t *pm, *pinv;                   /* each survivor's old slot; an old slot's new index  */
    uint32_t *pchg, *ptl;                 /* the changed slots; the tail's                      */
    Chains ch[3];                         /* the encoder's search: PS_OUT, PS_OLD, PS_EV        */
    RecBytes pctl, pref, psurv, pbirth, ptail;   /* the encoder's pool frame, per block         */
    RecBytes pb;                          /* the decoder's projectile records' bodies           */
    size_t pat[RECC_TICKS_MAX];           /* the decoder's: each iteration's control values     */
    /* the record frames: each one's state, and the frame of each section (-1: none) */
    void* frame[RECC_FRAMES];
    int fix[RCS_COUNT];
    unsigned char* fbody;                 /* the decoder's: a frame record's body, RECF_BODY_MAX */
    /* the kinds (THE KINDS): the run's patterns, front first; the last explicit empty feature
       tick, its root's body then its end's; whether an explicit RK_FEATURES came since the
       reset. The encoder's iteration as runs (u32 kind, count) and its pattern; the decoder's
       runs (u32 kind, count, RUN_* flags) and each iteration's first run. */
    RecBytes kmru[RECC_KINDS_MRU];
    unsigned nkmru;
    int kbad;                             /* a pattern failed an allocation or its bound         */
    unsigned char tpair[16];
    int tpaired, featseen;
    RecBytes ktok, kpat;
    size_t irun[RECC_TICKS_MAX + 1];
};

/* the sizes a codec is made for, each within its bound (tagpu_reccodec.h, reccodec_params) */
static int sizes_ok(const RecCodecParams* p)
{
    return p->nslots >= 2 && p->nslots <= 0xFFFF && p->nproj >= 1 && p->nproj <= 0xFFFF &&
           p->gw >= 1 && p->gw <= 0xFFFF && p->gh >= 1 && p->gh <= 0xFFFF &&
           (uint64_t)p->gw * p->gh <= RECC_GRID_MAX && p->nwrecks >= 1 &&
           p->nwrecks <= RECC_WRECKS_MAX;
}

RecCodec* reccodec_new(const RecCodecParams* p)
{
    RecCodec* c;
    unsigned i, nslots = p->nslots, nproj = p->nproj, rows;
    int ok = 1;
    if (!sizes_ok(p)) return NULL;
    rows = nproj > RECC_SHOTS_MAX ? nproj : RECC_SHOTS_MAX;
    c = (RecCodec*)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->nslots = nslots;
    c->nproj = nproj;
    c->gw = p->gw;
    c->gh = p->gh;
    c->nwrecks = p->nwrecks;
    for (i = 0; i < RCS_COUNT; i++) c->fix[i] = -1;
    for (i = 0; i < RECC_FRAMES; i++) {
        c->frame[i] = FRAMES[i]->make(p);
        c->fix[FRAMES[i]->section] = (int)i;
        ok &= c->frame[i] != NULL;
    }
    c->fbody = (unsigned char*)malloc(RECF_BODY_MAX);
    if (!ok || !c->fbody) {
        reccodec_free(c);
        return NULL;
    }
    c->pool = (uint32_t*)calloc(nproj, PIMG);
    c->pool_n = (uint32_t*)calloc(nproj, PIMG);
    c->pool_l2 = (uint32_t*)calloc(nproj, PIMG);
    c->pool_l2n = (uint32_t*)calloc(nproj, PIMG);
    c->pwin = (unsigned char*)calloc(nproj, PDW);
    c->pwin_n = (unsigned char*)calloc(nproj, PDW);
    c->page = (unsigned char*)calloc(nproj, 1);
    c->page_n = (unsigned char*)calloc(nproj, 1);
    c->lastnew = (uint32_t*)calloc(RECC_WEAPON_IDS + 1, PIMG);
    c->shots = (unsigned char*)calloc(RECC_SHOTS_MAX, SHOT);
    c->ev = (uint32_t*)calloc(RECC_SHOTS_MAX, PIMG);
    c->adv = (uint32_t*)calloc(nproj, PIMG);
    c->adv_done = (unsigned char*)calloc(nproj, 1);
    c->pm = (int32_t*)calloc(nproj, sizeof *c->pm);
    c->pinv = (int32_t*)calloc(nproj, sizeof *c->pinv);
    c->pchg = (uint32_t*)calloc(nproj, sizeof *c->pchg);
    c->ptl = (uint32_t*)calloc(nproj, sizeof *c->ptl);
    ok = c->pool && c->pool_n && c->pool_l2 && c->pool_l2n && c->pwin && c->pwin_n && c->page &&
         c->page_n && c->lastnew && c->shots && c->ev && c->adv && c->adv_done && c->pm && c->pinv &&
         c->pchg && c->ptl;
    /* a table of at least twice the rows, so a lookup's probes run at a load of at most 1/2 */
    for (i = 0; i < 3 && ok; i++) {
        Chains* x = &c->ch[i];
        unsigned k;
        for (x->tbits = 1; (1u << x->tbits) < 2 * rows; x->tbits++) ;
        x->tsize = 1u << x->tbits;
        x->prevw = (int32_t*)calloc(rows, sizeof *x->prevw);
        x->prevws = (int32_t*)calloc(rows, sizeof *x->prevws);
        x->used = (uint32_t*)calloc(rows, sizeof *x->used);
        x->top = (int32_t*)malloc(x->tsize * sizeof *x->top);
        x->key = (uint32_t*)calloc(x->tsize, sizeof *x->key);
        ok = x->prevw && x->prevws && x->used && x->top && x->key;
        if (ok)
            for (k = 0; k < x->tsize; k++) x->top[k] = -1;
    }
    if (!ok) {
        reccodec_free(c);
        return NULL;
    }
    c->airix = (uint16_t*)malloc(65536 * sizeof *c->airix);
    c->typix = (uint16_t*)malloc(65536 * sizeof *c->typix);
    c->unit = (uint32_t*)calloc(nslots, IMG);
    c->l2 = (uint32_t*)calloc(nslots, IMG);
    c->l3 = (unsigned char*)calloc(nslots, L3B);
    c->win = (unsigned char*)calloc(nslots, RECC_LANES_MAX);
    c->age = (unsigned char*)calloc(nslots, 1);
    c->rest = (unsigned char*)calloc(nslots, 1);
    c->lstep = (uint32_t*)calloc(nslots, 2 * sizeof(uint32_t));
    c->live = (unsigned char*)calloc(nslots, 1);
    c->touched = (unsigned char*)calloc(nslots, 1);
    c->jprev = (unsigned char*)calloc(nslots, 1);
    c->jlast = (unsigned char*)calloc(nslots, MB_MAX);
    c->jm = (unsigned char*)calloc(RECC_J2_MTF, MB_MAX);
    c->jnx = (int16_t*)calloc(RECC_J2_MTF, sizeof *c->jnx);
    c->jpv = (int16_t*)calloc(RECC_J2_MTF, sizeof *c->jpv);
    c->jhnext = (int16_t*)calloc(RECC_J2_MTF, sizeof *c->jhnext);
    c->jhead = (int16_t*)calloc(J2_HASH, sizeof *c->jhead);
    if (!c->airix || !c->typix || !c->unit || !c->l2 || !c->l3 || !c->win || !c->age || !c->rest || !c->live ||
        !c->touched || !c->lstep || !c->jprev || !c->jlast || !c->jm || !c->jnx || !c->jpv || !c->jhnext ||
        !c->jhead) {
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
    free(c->typix);
    free(c->unit);
    free(c->l2);
    free(c->l3);
    free(c->win);
    free(c->age);
    free(c->rest);
    free(c->lstep);
    free(c->live);
    free(c->touched);
    free(c->jprev);
    free(c->jlast);
    free(c->jm);
    free(c->jnx);
    free(c->jpv);
    free(c->jhnext);
    free(c->jhead);
    free(c->pool);
    free(c->pool_n);
    free(c->pool_l2);
    free(c->pool_l2n);
    free(c->pwin);
    free(c->pwin_n);
    free(c->page);
    free(c->page_n);
    free(c->lastnew);
    free(c->shots);
    free(c->ev);
    free(c->adv);
    free(c->adv_done);
    free(c->pm);
    free(c->pinv);
    free(c->pchg);
    free(c->ptl);
    for (i = 0; i < 3; i++) {
        free(c->ch[i].prevw);
        free(c->ch[i].prevws);
        free(c->ch[i].used);
        free(c->ch[i].top);
        free(c->ch[i].key);
    }
    rb_free(&c->pctl);
    rb_free(&c->pref);
    rb_free(&c->psurv);
    rb_free(&c->pbirth);
    rb_free(&c->ptail);
    rb_free(&c->pb);
    rb_free(&c->tl);
    rb_free(&c->dl);
    rb_free(&c->runs);
    rb_free(&c->lmask);
    rb_free(&c->lzz);
    rb_free(&c->ids);
    rb_free(&c->cols);
    rb_free(&c->jsym);
    rb_free(&c->jrw);
    rb_free(&c->jnew);
    rb_free(&c->jr);
    rb_free(&c->jw);
    rb_free(&c->ktok);
    rb_free(&c->kpat);
    for (i = 0; i < RECC_KINDS_MRU; i++) rb_free(&c->kmru[i]);
    for (i = 0; i < RCS_COUNT; i++) rb_free(&c->sec[i]);
    for (i = 0; i < RECC_FRAMES; i++)
        if (c->frame[i]) FRAMES[i]->drop(c->frame[i]);
    free(c->fbody);
    free(c);
}

void reccodec_invalidate(RecCodec* c) { c->valid = 0; }
int reccodec_valid(const RecCodec* c) { return c->valid; }
const uint32_t* reccodec_units(const RecCodec* c) { return c->unit; }
const unsigned char* reccodec_live(const RecCodec* c) { return c->live; }
const unsigned char* reccodec_pool(const RecCodec* c, unsigned* count, unsigned* hw)
{
    *count = c->pcount;
    *hw = c->phw;
    return (const unsigned char*)c->pool;
}

void reccodec_world(const RecCodec* c, RecFrameWorld* out)
{
    unsigned i;
    out->gw = c->gw;
    out->gh = c->gh;
    out->grid = NULL;
    for (i = 0; i < RECC_FRAMES; i++)
        if (FRAMES[i]->world) FRAMES[i]->world(c->frame[i], out);
}

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
    c->lstep[2 * s] = c->lstep[2 * s + 1] = 0;
}

static int pool_restart(RecCodec* c, const unsigned char* pool, unsigned count, unsigned hw);
static void listing_restart(RecCodec* c);

void reccodec_restart(RecCodec* c, const RecCodecParams* p, const uint32_t* units,
                      const unsigned char* live, const unsigned char* pool, unsigned count,
                      unsigned hw, const RecFrameWorld* world)
{
    unsigned s, j, i;
    /* BOUNDS: the world is the caller's, allocated at its header's grid size, and is taken only
       when that is the grid this codec was made for, so a frame copies exactly gw * gh *
       REC_CELL_BYTES bytes from it, the size both sides hold. Any other world leaves the codec
       invalid: a reader refuses the block ("a sectioned block with no frame to continue") and the
       writer declines it. A cell's bytes are data: a frame uses them as reference bytes, never as
       an index. */
    int wok = world && world->grid && world->gw == c->gw && world->gh == c->gh;
    c->valid = 0;
    /* A failed allocation latches its buffer's oom (rb_grow); some uses reset only its length, so
       a latch would decline every later block of a writer and refuse every later sectioned block
       of a reader. A restart is where both start again after a failure, so every working buffer
       starts here empty and unlatched. */
    rb_clear(&c->tl);
    rb_clear(&c->dl);
    rb_clear(&c->runs);
    rb_clear(&c->lmask);
    rb_clear(&c->lzz);
    rb_clear(&c->ids);
    rb_clear(&c->cols);
    rb_clear(&c->jsym);
    rb_clear(&c->jrw);
    rb_clear(&c->jnew);
    rb_clear(&c->jr);
    rb_clear(&c->jw);
    listing_restart(c);
    rb_clear(&c->pctl);
    rb_clear(&c->pref);
    rb_clear(&c->psurv);
    rb_clear(&c->pbirth);
    rb_clear(&c->ptail);
    rb_clear(&c->pb);
    rb_clear(&c->ktok);
    rb_clear(&c->kpat);
    for (i = 0; i < RECC_KINDS_MRU; i++) rb_clear(&c->kmru[i]);
    c->nkmru = 0;
    c->kbad = 0;
    memset(c->tpair, 0, sizeof c->tpair);
    c->tpaired = c->featseen = 0;
    for (i = 0; i < RCS_COUNT; i++) rb_clear(&c->sec[i]);
    for (i = 0; i < RECC_FRAMES; i++) FRAMES[i]->restart(c->frame[i], wok ? world : NULL);
    if (!wok || !p->usable || p->nslots != c->nslots || p->nproj != c->nproj || p->gw != c->gw ||
        p->gh != c->gh || p->nwrecks != c->nwrecks || p->nlanes < 1 ||
        p->nlanes > RECC_LANES_MAX || p->nair > RECC_AIR_MAX || p->ntypes > RECC_TYPES_MAX)
        return;
    if (p != &c->p) c->p = *p;
    for (i = 0; i < REC_LR_N; i++) c->lane_of[i] = -1;
    c->ns1 = c->ns2 = c->ncnt = 0;
    for (j = 0; j < c->p.nlanes; j++) {
        unsigned role = c->p.lane[j].role;
        if (role != REC_LR_PLAIN && role != REC_LR_COUNT_ && role < REC_LR_N) c->lane_of[role] = (int)j;
        /* the default winner: the rule where the role has one, else the previous value */
        c->defwin[j] = role == REC_LR_PLAIN || role == REC_LR_FW || role == REC_LR_FH ||
                       role == REC_LR_LAYER || role == REC_LR_TYPE || role == REC_LR_OWNER ||
                       role == REC_LR_BUILD_LEFT;
        if (c->defwin[j]) c->s1[c->ns1++] = (unsigned char)j;
        else if (role == REC_LR_VY || role == REC_LR_SPEED || role == REC_LR_AX ||
                 role == REC_LR_AY || role == REC_LR_AZ || role == REC_LR_BANK ||
                 role == REC_LR_ROLL || role == REC_LR_Y || role == REC_LR_SZ)
            c->s2[c->ns2++] = (unsigned char)j;
        else if (role == REC_LR_COUNT_ && c->ncnt < RECC_COUNTS_MAX)
            c->cnt[c->ncnt++] = (unsigned char)j;
    }
    for (i = 1; i < REC_LR_N; i++)
        if (i != REC_LR_COUNT_ && c->lane_of[i] < 0) return;   /* reccodec_params refuses such a table */
    memset(c->defwin + c->p.nlanes, 1, RECC_LANES_MAX - c->p.nlanes);
    {
        int64_t g = c->p.gravity < 0 ? 0 : c->p.gravity > (1 << 24) ? (1 << 24) : c->p.gravity;
        c->q = g * 65536 / 0xCCD;
    }
    memset(c->lsettled, 0, sizeof c->lsettled);
    memset(c->lstep, 0, (size_t)c->nslots * 2 * sizeof(uint32_t));
    for (i = 0; i < 65536; i++) c->airix[i] = c->typix[i] = NOROW;
    for (i = 0; i < c->p.nair; i++) c->airix[c->p.air[i].type] = (uint16_t)i;
    for (i = 0; i < c->p.ntypes; i++) c->typix[c->p.types[i].type] = (uint16_t)i;
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
    c->valid = pool_restart(c, pool, count, hw);
}

/* ---- the header's codec entries ------------------------------------------------------------- */

/* reccodec_params marks the roles it has seen in one qword */
typedef char rec_roles_fit_a_qword[REC_LR_N <= 64 ? 1 : -1];

int reccodec_params(RecCodecParams* p, unsigned nslots, unsigned nproj, unsigned gw, unsigned gh,
                    unsigned nwrecks, unsigned ndefs, const RecCodecEntries* e, char* why,
                    unsigned cap)
{
    const void* codec = e->p[RECC_E_CODEC];
    const void* physics = e->p[RECC_E_PHYSICS];
    const void* air = e->p[RECC_E_AIR];
    const void* types = e->p[RECC_E_TYPES];
    unsigned ncodec = e->n[RECC_E_CODEC], nphysics = e->n[RECC_E_PHYSICS], nair = e->n[RECC_E_AIR];
    unsigned ntypes = e->n[RECC_E_TYPES];
    memset(p, 0, sizeof *p);
    p->nslots = nslots;
    p->nproj = nproj;
    p->gw = gw;
    p->gh = gh;
    p->nwrecks = nwrecks;
    if (nslots < 2 || nslots > 0xFFFF) REFUSE("RT_SLOTS %u is outside 2..65535", nslots);
    if (nproj < 1 || nproj > 0xFFFF) REFUSE("RT_PROJECTILES %u is outside 1..65535", nproj);
    if (codec) {
        const unsigned char* b = (const unsigned char*)codec;
        unsigned n, i, at = 0, counts = 0;
        uint64_t seen = 0;
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
                seen |= (uint64_t)1 << role;
            }
            p->lane[i].off = (uint16_t)off;
            p->lane[i].width = (uint8_t)width;
            p->lane[i].role = (uint8_t)role;
            at += width;
        }
        if (at != IMG) REFUSE("RT_CODEC's rows cover %#x bytes, not the image's %#x", at, IMG);
        for (i = 1; i < REC_LR_N; i++)
            if (i != REC_LR_COUNT_ && !(seen >> i & 1)) REFUSE("RT_CODEC has no row for role %u", i);
        p->nlanes = n;
    }
    if (physics) {
        const unsigned char* b = (const unsigned char*)physics;
        if (nphysics != 12) REFUSE("RT_PHYSICS is %u bytes, not 12", nphysics);
        if (b[5] > 1) REFUSE("RT_PHYSICS's sight rule is %u, not 0 or 1", b[5]);
        if (get16(b + 6)) REFUSE("RT_PHYSICS's pad is not zero");
        p->gravity = (int32_t)get32(b);
        p->sea = b[4];
        p->sight = b[5];
        p->bcols = get32(b + 8);
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
    if (types) {
        /* BOUND: rows and types below RT_UNITDEFS' count, the defs the capture read them from */
        const unsigned char* b = (const unsigned char*)types;
        uint32_t n;
        unsigned i;
        if (ntypes < REC_TYPES_HEAD) REFUSE("RT_TYPES is %u bytes, under its %u-byte head", ntypes, REC_TYPES_HEAD);
        n = get32(b);
        if (b[4 + REC_SLOPES]) REFUSE("RT_TYPES' pad is not zero");
        if (n > RECC_TYPES_MAX || n > ndefs)
            REFUSE("RT_TYPES has %lu rows, past %u or RT_UNITDEFS' %u", (unsigned long)n, RECC_TYPES_MAX, ndefs);
        if (ntypes != REC_TYPES_HEAD + 20 * n) REFUSE("RT_TYPES is %u bytes, not %lu for %lu rows", ntypes,
                                                    (unsigned long)(REC_TYPES_HEAD + 20 * n), (unsigned long)n);
        memcpy(p->slope, b + 4, REC_SLOPES);
        for (i = 0; i < n; i++) {
            const unsigned char* r = b + REC_TYPES_HEAD + 20 * i;
            RecTypeRow* t = &p->types[i];
            t->type = (uint16_t)get16(r);
            t->sight_h = r[2];
            t->zero = r[3];
            if (t->zero) REFUSE("RT_TYPES row %u's pad is not zero", i);
            if (t->type >= ndefs) REFUSE("RT_TYPES row %u's type %u is past RT_UNITDEFS' %u", i, t->type, ndefs);
            if (i && t->type <= p->types[i - 1].type)
                REFUSE("RT_TYPES row %u's type %u does not follow %u", i, t->type, p->types[i - 1].type);
            t->maxhp = get32(r + 4);
            t->maxv = (int32_t)get32(r + 8);
            t->brake = (int32_t)get32(r + 12);
            t->acc = (int32_t)get32(r + 16);
        }
        p->ntypes = n;
    }
    /* a grid or a wreck pool past the record frames' bounds turns the codec off, never the
       header unreadable: such a segment's stream is records */
    p->usable = codec && physics && sizes_ok(p);
    if (!p->usable && why && cap) {
        if (!codec || !physics)
            snprintf(why, cap, "the header has no RT_CODEC and RT_PHYSICS");
        else
            snprintf(why, cap, "a feature grid of %u x %u cells or a wreck pool of %u records past "
                     "the codec's bounds", gw, gh, nwrecks);
    }
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
    unsigned age, s;
    int dec;
    /* the decoder's residuals: the listed unit's mask (NULL: not listed) and the columns */
    const unsigned char* mask;
    const unsigned char* sec;
    size_t* colat;
    const size_t* colend;                 /* each lane's column ends there                       */
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
            if (!varint_get(x->sec, x->colend[j], &x->colat[j], &z)) return 0;
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

/* The ledger's requested and granted (exe-reverse-engineering.md, the ledger), energy then metal.
   The build, repair and stockpile steps add one request a tick as a float (0x4011C0, 0x401180):
   the requested grows by the same step e from the first nonzero value after a zero, and the
   owner's settle (0x401B3D) zeroes it every 30 ticks for a unit with +0x110 bit 28. The granted
   adds the same e while the unit's own debts +0xC8 and +0xE0 are not above 0 (either refuses both
   in 0x4011C0), so it equals this tick's requested whenever it equalled the last tick's. The
   candidates: requested is 0 at the owner's settle, L1 + e while it grows, L1 when it held and
   e itself after a zero that followed a value; granted is this tick's requested when the two
   were equal, else L1. The settle is learned per owner ID from the last requested that fell to
   zero, every slot of an owner settling together; e per slot from the first value after a zero.
   Each lane's winner is the rule, always. A prediction is only ever a size question: a wrong one
   costs a residual. */

/* a + b as IEEE single-precision values, rounded to nearest even, in integers: the sum the FPU
   gives at any precision control (64 and 53 bits round a sum of two singles once more without a
   difference, 2p + 2 <= 53), and the same in every thread whatever its control word. An infinity
   or a NaN gives a: the prediction only has to be the same on both sides. */
uint32_t reccodec_f32_add(uint32_t a, uint32_t b)
{
    uint32_t sa = a >> 31, sb = b >> 31, ea = a >> 23 & 0xFF, eb = b >> 23 & 0xFF;
    uint32_t ma = a & 0x7FFFFF, mb = b & 0x7FFFFF, m, t, low;
    if (ea == 0xFF || eb == 0xFF) return a;
    if (ea) ma |= 0x800000; else ea = 1;
    if (eb) mb |= 0x800000; else eb = 1;
    ma <<= 3;                                   /* three bits below: guard, round, sticky */
    mb <<= 3;
    if (ea < eb || (ea == eb && ma < mb)) {
        t = sa, sa = sb, sb = t;
        t = ea, ea = eb, eb = t;
        t = ma, ma = mb, mb = t;
    }
    t = ea - eb;
    if (t > 27) mb = mb != 0;
    else if (t) mb = mb >> t | ((mb & ((1u << t) - 1)) != 0);
    if (sa == sb) {
        m = ma + mb;
        if (m >> 27) {
            m = m >> 1 | (m & 1);
            ea++;
        }
    } else {
        m = ma - mb;
        if (!m) return 0;
        while (m < (1u << 26) && ea > 1) {
            m <<= 1;
            ea--;
        }
    }
    low = m & 7;
    m >>= 3;
    if (low > 4 || (low == 4 && (m & 1))) m++;
    if (m >> 24) {
        m >>= 1;
        ea++;
    }
    if (ea >= 0xFF) return sa << 31 | 0x7F800000u;
    return sa << 31 | (m >> 23 ? ea : 0) << 23 | (m & 0x7FFFFF);
}

static int ledger_steps(Eval* x)
{
    RecCodec* c = x->c;
    uint32_t own = lane_get(x->u, &c->p.lane[c->lane_of[REC_LR_OWNER]]);
    int known = own < RECC_OWNERS && c->lsettled[own];
    int settle = known && (x->gt - c->lsettle[own]) % 30u == 0;
    static const int req[2] = { REC_LR_EREQ, REC_LR_MREQ }, grant[2] = { REC_LR_EGRANT, REC_LR_MGRANT };
    unsigned k;
    for (k = 0; k < 2; k++) {
        unsigned jr = (unsigned)c->lane_of[req[k]], jg = (unsigned)c->lane_of[grant[k]];
        const RecLaneRow* rr = &c->p.lane[jr];
        uint32_t l1 = lane_get(x->l1, rr), l2 = lane_get(x->l2, rr), *e = &c->lstep[2 * x->s + k];
        uint32_t p, ur, g1 = lane_get(x->l1, &c->p.lane[jg]);
        if (l1 && settle) p = 0;
        else if (!l1) p = l2 ? *e : 0;
        else if (l1 == l2) p = l1;
        else p = reccodec_f32_add(l1, *e);
        x->win[jr] = 0;
        if (!step(x, jr, 1, p, 0, 0)) return 0;
        x->win[jr] = 0;
        ur = lane_get(x->u, rr);
        if (!l1 && ur) *e = ur;
        if (l1 && !ur && own < RECC_OWNERS) {
            c->lsettle[own] = x->gt;
            c->lsettled[own] = 1;
        }
        x->win[jg] = 0;
        if (!step(x, jg, 1, !ur ? 0 : g1 == l1 ? ur : g1, 0, 0)) return 0;
        x->win[jg] = 0;
    }
    return 1;
}

#define LANE(role)  (&c->p.lane[c->lane_of[role]])
#define U_(role)    lane_get(x->u, LANE(role))
#define L1_(role)   lane_get(x->l1, LANE(role))
#define L2_(role)   lane_get(x->l2, LANE(role))
#define STEP(role, rl, c0, h3, c3) do { if (!step(x, (unsigned)c->lane_of[role], rl, c0, h3, c3)) return 0; } while (0)

/* trunc(f * m) for the IEEE single whose bits are f, exactly: 1 and *out, or 0 for an f outside
   [0, 1] -- a sign bit, past 1.0, an infinity or a NaN. f is mant * 2^(e - 150), the implicit bit
   set on a normal and e taken as 1 on a denormal; e <= 127 puts the shift at 23 or more, and
   mant * m < 2^24 * 2^32. The build step's truncation 0x4E43A0 (`fistp` under a chop control
   word, DISASSEMBLED) of the product at the FPU's precision [INFERRED: 53 bits, the MSVC
   runtime's default _PC_53] is this value whenever m < 2^29; past it the product rounds first, which costs a residual. */
static int ex_fm(uint32_t f, uint32_t m, uint64_t* out)
{
    uint32_t e = f >> 23 & 0xFF, mant = f & 0x7FFFFF;
    unsigned sh;
    if (f > 0x3F800000u) return 0;
    if (e) mant |= 0x800000;
    else e = 1;
    sh = 150 - e;
    *out = sh >= 64 ? 0 : ((uint64_t)mant * m) >> sh;
    return 1;
}

/* the RT_TYPES row of an image's type, NULL for a type without one */
static const RecTypeRow* type_row(const RecCodec* c, const unsigned char* img)
{
    unsigned t = c->typix[lane_get(img, &c->p.lane[c->lane_of[REC_LR_TYPE]])];
    return t < c->p.ntypes ? &c->p.types[t] : NULL;
}

/* The unit tick's HP percentage (0x48AE77..0x48AEA3, DISASSEMBLED): movsx(+0x108) * 100 in 32
   bits, divided unsigned by def +0x1FA (m, not 0), clamped as a signed value to 0..100. */
static uint32_t hp_pct(uint32_t hp, uint32_t m)
{
    uint32_t q = (uint32_t)(int32_t)s16(hp) * 100u / m;
    return (int32_t)q < 0 ? 0 : (int32_t)q > 100 ? 100 : q;
}

/* The unit tick writes the HP percentage when GameTime is a multiple of 30 (0x48AE5F..0x48AE75,
   DISASSEMBLED), and the iteration labelled gt holds step gt - 1 */
static int hp_phase(uint32_t gt) { return (gt - 1u) % 30u == 0; }

/* The hit points and their percentages, for a type whose row has a maximum M. The build step
   0x41BA60 takes the fraction left from fo (+0x104, read at 0x41BA6D) to fn and adds
   trunc(fo * M) - trunc(fn * M) to the HP (0x41BB26..0x41BB90, M the def's +0x1FA as a u32);
   building (fn < fo) it caps the sum at M by an unsigned compare (0x41BC7F), unbuilding it floors
   it at 0 (0x41BC14..0x41BC22), and it stores the HP only when it stores the fraction, so an
   unchanged fraction predicts an unchanged HP. Every 30 ticks the unit tick moves +0xF6 to +0xF7
   and writes the percentage of the HP to +0xF6 (0x48AEA3..0x48AEAF), its only writer in play
   (the others are the creates 0x485B67 and the saved game's loader 0x487296, DISASSEMBLED), so
   the two lanes' winner is the rule, always. DISASSEMBLED. */
static int hp_steps(Eval* x, uint32_t m)
{
    RecCodec* c = x->c;
    uint32_t fo = L1_(REC_LR_BUILD_LEFT), fn = U_(REC_LR_BUILD_LEFT), hp = L1_(REC_LR_HP);
    uint64_t a, b;
    unsigned j6 = (unsigned)c->lane_of[REC_LR_HPPCT], j7 = (unsigned)c->lane_of[REC_LR_HPPCT30];
    int phase = hp_phase(x->gt);
    if (fo != fn && ex_fm(fo, m, &a) && ex_fm(fn, m, &b)) {
        /* the engine's sum is 32 bits: a and b are at most m < 2^32 */
        int64_t v = s32(s16(hp) + (int64_t)a - (int64_t)b);
        if (fn < fo) {                  /* both in [0, 1]: their bits order them */
            if ((uint32_t)v >= m) v = m;
        } else if (v <= 0) {
            v = 0;
        }
        hp = (uint32_t)v;
    }
    STEP(REC_LR_HP, 1, hp, 0, 0);
    x->win[j7] = 0;
    STEP(REC_LR_HPPCT30, 1, phase ? L1_(REC_LR_HPPCT) : L1_(REC_LR_HPPCT30), 0, 0);
    x->win[j7] = 0;
    x->win[j6] = 0;
    STEP(REC_LR_HPPCT, 1, phase ? hp_pct(U_(REC_LR_HP), m) : L1_(REC_LR_HPPCT), 0, 0);
    x->win[j6] = 0;
    return 1;
}

/* A ground unit's speed: the ground step 0x43CC20 adds the caller's step to it -- the def's
   acceleration +0x19E or minus its brake rate +0x19A (0x43D0AC..0x43D0B8, 0x43CD46..0x43CD4C) --
   floors it at 0 (0x43CC37..0x43CC3B), then caps it at mv = s32(SLOPE[sl] * maxv) / 100, sl =
   clamp(movsx(+0x68) >> 11, -5, 5), SLOPE the signed bytes at 0x505205 + sl, maxv the def's
   +0x192, a 64-bit product shifted by 16 each way and a signed divide by 100 << 16
   (0x43CC42..0x43CC95, the cap 0x43CCD3..0x43CCD8), DISASSEMBLED. The movement pass runs the step
   before the placement (the ground step's call 0x43DD4D, the placement's 0x43DD55), so sl comes from L1's +0x68, the value the tick
   found; MEASURED on the heavy hour, 2026-10-12, 1.75 KB smaller than U's. The two steps are
   candidates 0 and 3. Not modelled: the halving of mv in water (0x43CCA0..0x43CCCE: +0x70 below the sea level
   and def +0x241 & 0x81000 clear), which leaves a residual. A type with no speed (mv 0) is not
   stepped: both candidates are the last value. */
static int ground_speed(Eval* x, const RecTypeRow* t)
{
    RecCodec* c = x->c;
    int64_t sl = asr(s16(L1_(REC_LR_ROLL)), 11), mv, l1 = s32(L1_(REC_LR_SPEED)), a, b;
    sl = sl < -5 ? -5 : sl > 5 ? 5 : sl;
    mv = s32((int64_t)(int8_t)c->p.slope[sl + 5] * t->maxv) / 100;
    if (!mv) {
        a = b = l1;
    } else {
        a = s32(l1 + t->acc);
        b = s32(l1 - (int64_t)t->brake);
        a = a < 0 ? 0 : a;              /* floored, then capped: an mv below 0 caps too */
        b = b < 0 ? 0 : b;
        a = a > mv ? mv : a;
        b = b > mv ? mv : b;
    }
    STEP(REC_LR_SPEED, 1, (uint32_t)(uint64_t)a, 1, (uint32_t)(uint64_t)b);
    return 1;
}

/* The steps of one slot, in an order where each rule reads only lanes decoded before it:
   S1 every row's generic lanes; S2 a ground row's generic lanes that an aircraft predicts by
   rule, and a ground unit's speed; S3 the turn rate, the hit points and the heading; S4 the
   velocity; S5 an aircraft's speed, acceleration and bank; S6 the position; S7 the cells, the
   bucket, the stamps, the countdowns and the sight; S8 the ledger, which reads its owner from S1.
   A rule of a type's row (RT_TYPES) is on only for a type with one. 1, or 0 when a residual
   cannot be read. */
static int frame_slot(Eval* x)
{
    RecCodec* c = x->c;
    unsigned i, ai, layer, layer1;
    int air, ground, airborne, landing, sight = c->p.sight;
    int64_t vx, vy, vz;
    const RecTypeRow* t;
    for (i = 0; i < c->ns1; i++)
        if (!step(x, c->s1[i], 0, 0, 0, 0)) return 0;
    ai = c->airix[U_(REC_LR_TYPE)];
    air = ai != NOROW && ai < c->p.nair;
    t = type_row(c, x->u);
    ground = !air && t;
    if (!air)
        for (i = 0; i < c->ns2; i++) {
            unsigned j = c->s2[i];
            if (ground && (j == (unsigned)c->lane_of[REC_LR_SPEED] || j == (unsigned)c->lane_of[REC_LR_SZ]))
                continue;
            if (!step(x, j, 0, 0, 0, 0)) return 0;
        }
    if (ground && !ground_speed(x, t)) return 0;
    /* S3: the turn rate's one-tick blip: a rate one away from the last returns to the one
       before (MEASURED, no engine writer traced: research/notes/tadr-port/demo-recorder-size.md,
       *P4*, 76 KB of the heavy hour); its winner is the rule, always */
    {
        uint32_t t1 = L1_(REC_LR_TURN), t2 = L2_(REC_LR_TURN);
        int64_t d = s16(t1 - t2);
        unsigned jt = (unsigned)c->lane_of[REC_LR_TURN];
        x->win[jt] = 0;
        STEP(REC_LR_TURN, 1, d >= -1 && d <= 1 ? t2 : t1, 0, 0);
        x->win[jt] = 0;
    }
    if (t && t->maxhp) {
        if (!hp_steps(x, t->maxhp)) return 0;
    } else {
        STEP(REC_LR_HP, 0, 0, 0, 0);
        STEP(REC_LR_HPPCT30, 0, 0, 0, 0);
        STEP(REC_LR_HPPCT, 0, 0, 0, 0);
    }
    /* heading += turn rate (0x43CFD1..0x43CFDB, DISASSEMBLED; research/sizelab/units) */
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
    /* S6: position += velocity. The placement 0x43D6D0, which the movement pass calls after
       either step (0x43DD55), adds the velocity to x, y and z for every uncarried unit (the carried
       test 0x43D6DD..0x43D6E5 is the only branch before it; y loaded at 0x43D80D, added at
       0x43D813) and stores the three on each path that changes them (0x43D8C5..0x43D8D0,
       0x43D9F1..0x43D9F7, 0x43DA20..0x43DA35), DISASSEMBLED. Predicting y only for an aircraft is
       the codec's choice: a ground unit's altitude is a generic lane. */
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
    /* the sort bucket: when the footprint's cell or the layer moved, the footprint stamp 0x47CC30
       (called where the cell moves, as the claim stamp below) takes the bucket at
       ((z >> 23) * columns + (x >> 23)) once 0 <= fx, 0 <= fz, fx + fw <= W and fz + fh <= H, the
       grid's columns and rows (0x47CC57..0x47CCD8, DISASSEMBLED; `<=` because Impure's always-on
       fix_last_cell turns the two edge tests at 0x47CC8B and 0x47CCA3 into `jg`, tagpu_patches.c).
       A footprint that fails a test goes to 0x47D073/0x47D079, which files the unit in the
       off-map bucket *(main+0x142B7) (stored at 0x47D0CF, DISASSEMBLED), REC_ID_OFFMAP in the
       image; the image carries any other bucket as its index + 1. Unmoved, it keeps its bucket.
       Not predicted: fix_last_cell's stub clamps a linear index outside [0, rows * columns) into
       the grid, and RT_PHYSICS carries no row count, so a clamped bucket is a residual.
       RT_PHYSICS's columns 0 makes the lane generic. |z >> 23| < 2^9 and the columns < 2^32, so
       the product < 2^41. */
    {
        int64_t fx = s16(U_(REC_LR_FX)), fz = s16(U_(REC_LR_FZ));
        int moved = fx != s16(L1_(REC_LR_FX)) || fz != s16(L1_(REC_LR_FZ)) || layer != layer1;
        int in = fx >= 0 && fz >= 0 && fx + s16(U_(REC_LR_FW)) <= (int64_t)c->p.gw &&
                 fz + s16(U_(REC_LR_FH)) <= (int64_t)c->p.gh;
        int64_t b = asr(s32(U_(REC_LR_Z)), 23) * (int64_t)c->p.bcols + asr(s32(U_(REC_LR_X)), 23) + 1;
        STEP(REC_LR_BUCKET, c->p.bcols != 0,
             !moved ? L1_(REC_LR_BUCKET) : in ? (uint32_t)(uint64_t)b : REC_ID_OFFMAP, 0, 0);
    }
    /* the claim stamp: GameTime when the footprint or the layer moved (0x440A83..0x440A91 writes
       main+0x38A47 as a dword, DISASSEMBLED); the iteration labelled gt holds step gt - 1. Other
       writers -- the footprint stamp 0x47CC54, the pathfinder 0x440B30 and its restore 0x440BC4,
       the mover's constructor (0x43DC16) and restore (0x43DEC5) -- are not predicted: their
       stamps cost residuals, size only. */
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
    if (air || ground) {
        /* the sight z: ((z >> 16) - h / 2) >> 5 with h = clamp(altitude + def +0x170, 0, 255)
           (0x4825D8..0x48261C), the caller 0x4827B0 raising any unit's altitude to
           (sea + 1) << 16 (0x48280B..0x48281B) and reading the def's byte at 0x4827FB,
           DISASSEMBLED: an aircraft's byte from its RT_AIR row, a ground unit's from its
           RT_TYPES row */
        int64_t y = s32(U_(REC_LR_Y)), floor_y = ((int64_t)c->p.sea + 1) * 65536, h;
        if (y < floor_y) y = floor_y;
        h = s16(asr(y, 16)) + (air ? c->p.air[ai].sight_h : t->sight_h);
        h = h < 0 ? 0 : h > 255 ? 255 : h;
        STEP(REC_LR_SZ, sight, (uint32_t)(uint64_t)asr(asr(s32(U_(REC_LR_Z)), 16) - (h >> 1), 5), 0, 0);
    }
    return ledger_steps(x);
}

/* After evaluating candidate s at gt, with U its image: the state advances, and the slot rests
   when it is a fixed point.

   THE FIXED POINT. If the history is full (age 3), U == L1 == L2, L3 equals U's lanes, and the
   unit either has no velocity or a move stamp older than gt - 1, the ledger's four lanes are
   zero, and -- for a type whose RT_TYPES row has a maximum -- +0xF7 == +0xF6 == the percentage
   of its HP, then at gt + 1 every input of
   every candidate -- L1, L2, L3, age, and U for the lanes before each -- equals its value at gt.
   The dependences on gt are the move stamp's gt - 1 when it moved, which the second clause
   makes inexact at gt and at every later gt, and the HP percentage's phase, whose two
   predictions are then the lanes' own values at every gt; the claim stamp and the bucket do not
   move when U == L1, and the
   ledger's candidates are all 0 for a slot whose lanes are 0, whatever its owner's settle, and
   change neither its step nor the settle. So
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
    if (lane_get(u, &c->p.lane[c->lane_of[REC_LR_EREQ]]) || lane_get(u, &c->p.lane[c->lane_of[REC_LR_EGRANT]]) ||
        lane_get(u, &c->p.lane[c->lane_of[REC_LR_MREQ]]) || lane_get(u, &c->p.lane[c->lane_of[REC_LR_MGRANT]]))
        rest = 0;
    if (rest) {
        const RecTypeRow* t = type_row(c, u);
        uint32_t f6 = lane_get(u, &c->p.lane[c->lane_of[REC_LR_HPPCT]]);
        if (t && t->maxhp && (lane_get(u, &c->p.lane[c->lane_of[REC_LR_HPPCT30]]) != f6 ||
                              hp_pct(lane_get(u, &c->p.lane[c->lane_of[REC_LR_HP]]), t->maxhp) != f6))
            rest = 0;
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

/* ---- the listing ---------------------------------------------------------------------------

   THE LISTING (tagpu_recfmt.h, RCS_UNITS): which units an iteration lists and each one's lane
   mask, from state both sides keep. An iteration's CANDIDATES are the slots live at its start
   that no RK_UNIT or RK_UNIT_GONE names and that do not rest; its WOKEN slots are resting ones
   with a delta, which the frame evaluates too and always lists (a resting slot's prediction is
   its copy, so a delta leaves a residual). A candidate the iteration before listed is CARRIED and
   takes a symbol: 0 not listed, 1 listed with its own last mask, 2 listed with another; the
   other listed candidates are ranks among the candidates not carried, and the woken slots are
   slots. Every listed unit but a symbol 1 takes an id: 0 its slot's last mask (not for a carried
   one, and only when that mask is not empty), k the k-th of the run's masks most recent first,
   the list's length + 1 the block's next new mask. Every listed mask then goes to the front of
   the list -- inserted when absent, the least recent dropped when the list holds RECC_J2_MTF --
   so the list never holds a mask twice: the decoder refuses a new mask the list already has.
   The previous listed set, the last masks and the list are state of the run, cleared at every
   reset (listing_restart); the slot's last mask survives a birth in its slot, which costs size
   only. The finds go through a hash of chains and a move to the front costs the mask's rank, so
   the work is a function of the ranks the file names, each bounded by RECC_J2_MTF. */

static void listing_restart(RecCodec* c)
{
    unsigned i;
    memset(c->jprev, 0, c->nslots);
    memset(c->jlast, 0, (size_t)c->nslots * MB_MAX);
    for (i = 0; i < J2_HASH; i++) c->jhead[i] = -1;
    c->jfront = c->jtail = -1;
    c->njm = 0;
}

static uint32_t fnv(const unsigned char* p, unsigned n)
{
    uint32_t h = 2166136261u;
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

static unsigned j2_hash(const unsigned char* m) { return fnv(m, MB_MAX) & (J2_HASH - 1); }

/* the stored mask equal to m, or -1 */
static int j2_find(const RecCodec* c, const unsigned char* m)
{
    int k = c->jhead[j2_hash(m)];
    while (k >= 0 && memcmp(c->jm + (size_t)k * MB_MAX, m, MB_MAX)) k = c->jhnext[k];
    return k;
}

static void j2_unlink(RecCodec* c, int k)
{
    if (c->jpv[k] >= 0) c->jnx[c->jpv[k]] = c->jnx[k];
    else c->jfront = c->jnx[k];
    if (c->jnx[k] >= 0) c->jpv[c->jnx[k]] = c->jpv[k];
    else c->jtail = c->jpv[k];
}

static void j2_push(RecCodec* c, int k)
{
    c->jpv[k] = -1;
    c->jnx[k] = (int16_t)c->jfront;
    if (c->jfront >= 0) c->jpv[c->jfront] = (int16_t)k;
    else c->jtail = k;
    c->jfront = k;
}

/* m to the front of the list, inserted when absent: the least recent dropped at RECC_J2_MTF */
static void j2_touch(RecCodec* c, const unsigned char* m)
{
    int k = j2_find(c, m);
    unsigned h;
    if (k >= 0) {
        j2_unlink(c, k);
        j2_push(c, k);
        return;
    }
    if (c->njm < RECC_J2_MTF) {
        k = (int)c->njm++;
    } else {
        int16_t* at;
        k = c->jtail;
        j2_unlink(c, k);
        at = &c->jhead[j2_hash(c->jm + (size_t)k * MB_MAX)];
        while (*at != k) at = &c->jhnext[*at];        /* k is on its chain: it was stored there */
        *at = c->jhnext[k];
    }
    memcpy(c->jm + (size_t)k * MB_MAX, m, MB_MAX);
    h = j2_hash(m);
    c->jhnext[k] = c->jhead[h];
    c->jhead[h] = (int16_t)k;
    j2_push(c, k);
}

static int mask_any(const unsigned char* m)
{
    unsigned i, any = 0;
    for (i = 0; i < MB_MAX; i++) any |= m[i];
    return any != 0;
}

/* The encoder's id of a listed mask m (THE LISTING), the list moved on: 0 for the slot's last
   mask `last` when `may0`, else 1 + its rank, else a new mask (the list's length + 1), which goes
   to the block's new masks */
static uint32_t j2_id(RecCodec* c, const unsigned char* m, const unsigned char* last, int may0)
{
    unsigned mb = (c->p.nlanes + 7) / 8;
    uint32_t r = 0;
    int k, q;
    if (may0 && mask_any(last) && !memcmp(m, last, MB_MAX)) {
        j2_touch(c, m);
        return 0;
    }
    k = j2_find(c, m);
    if (k < 0) {
        r = c->njm + 1;
        rb_put(&c->jnew, m, mb);
        j2_touch(c, m);
        return r;
    }
    for (q = c->jfront; q != k; q = c->jnx[q]) r++;
    j2_unlink(c, k);
    j2_push(c, k);
    return r + 1;
}

/* The decoder's mask for id v (THE LISTING) into out, the list moved on; the block's new masks
   `fresh` of which *next are taken. 0: v names no mask. */
static int j2_mask(RecCodec* c, uint32_t v, const unsigned char* last, int may0,
                   const unsigned char* fresh, unsigned nfresh, unsigned* next, unsigned char* out)
{
    unsigned mb = (c->p.nlanes + 7) / 8;
    if (v == 0) {
        if (!may0 || !mask_any(last)) return 0;
        memcpy(out, last, MB_MAX);
    } else if (v <= c->njm) {
        int k = c->jfront;
        while (--v) k = c->jnx[k];
        memcpy(out, c->jm + (size_t)k * MB_MAX, MB_MAX);
    } else if (v == c->njm + 1 && *next < nfresh) {
        memset(out, 0, MB_MAX);
        memcpy(out, fresh + (size_t)(*next)++ * mb, mb);
        if (j2_find(c, out) >= 0) return 0;      /* BOUND: the list never holds a mask twice */
    } else {
        return 0;
    }
    j2_touch(c, out);
    return 1;
}

/* ---- the pool frame ------------------------------------------------------------------------- */

/* A reference's section (tagpu_recfmt.h, RCS_PROJ): the new pool so far, the old pool, the old
   pool advanced one step, the rows made from the last iteration's shot events. */
enum { PS_OUT, PS_OLD, PS_ADV, PS_EV };

/* The survivor step's dword order, which the format fixes: each rule reads only dwords decoded
   before it -- the velocity (7..9) reads the heading, pitch and speed (13..15), the position
   (1..3) the velocity. */
static const unsigned char PORDER[PDW] = { 13, 14, 15, 7, 8, 9, 0, 1, 2, 3, 4, 5, 6, 10, 11, 12,
                                           16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 };

/* The encoder's search, policy and not format: the decoder reads only the section and the row a
   code names, so another choice here costs size, never readability. */
#define RECC_CAND          4    /* candidates a section at most                                */
#define RECC_MATCH_WINDOW  8    /* the old slots the matcher tries after its last pairing      */
#define RECC_MATCH_EQUAL   12   /* the dwords of 27 a pairing needs equal, +0x00 among them    */
#define RECC_TAIL_WINDOW   16   /* a tail slot's rows: within this of the section's anchor     */
#define RECC_PROBE_MAX     32   /* the slots a (weapon, shooter) lookup probes at most         */

/* a signed count or row distance's zigzag, |d| < 2^31 */
static uint32_t zz64(int64_t d) { return (uint32_t)(d >= 0 ? 2 * d : -2 * d - 1); }
static int64_t unzz64(uint32_t v) { return v & 1 ? -(int64_t)(v >> 1) - 1 : (int64_t)(v >> 1); }
static unsigned leblen(uint32_t v)
{
    return 1u + (v >= 1u << 7) + (v >= 1u << 14) + (v >= 1u << 21) + (v >= 1u << 28);
}

/* THE GUIDED RULE. The velocity the projectile tick gives a weapon with +0x111 bit 20 in flight
   (0x49BA74..0x49BABE, DISASSEMBLED), after its speed step (0x49BA16..0x49BA31) and its aim and
   turn (0x49BA34..0x49BA6F): vy = 0x4B70EF(pitch +0x38, speed +0x3A), h = 0x4B7123(pitch,
   speed), vx = -0x4B70EF(heading +0x36, h), vz = -0x4B7123(heading, h) -- dwords 7..9 from the
   heading (dword 13's high half), the pitch (dword 14's low half) and the speed (dword 14's high
   half and dword 15's low half). |SIN * speed| and |SIN * h| are at most 2^13 * 2^31. */
static void guided(const uint32_t* x, uint32_t v[3])
{
    uint32_t heading = x[13] >> 16, pitch = x[14] & 0xFFFF;
    int64_t speed = s32((int64_t)((x[14] >> 16) | (x[15] & 0xFFFF) << 16));
    int64_t h = cosmul(pitch, speed);
    v[0] = (uint32_t)(uint64_t)-sinmul(heading, h);
    v[1] = (uint32_t)(uint64_t)sinmul(pitch, speed);
    v[2] = (uint32_t)(uint64_t)-cosmul(heading, h);
}

/* The image's own velocity follows the guided rule -- a guided shot in flight, and the zero row.
   It stands in for the weapon bit the codec does not have. A predictor: wrong, it costs size. */
static int held(const uint32_t* x)
{
    uint32_t v[3];
    guided(x, v);
    return v[0] == x[7] && v[1] == x[8] && v[2] == x[9];
}

/* THE SURVIVOR STEP of old slot m (its last image L = P[m], the one before L2 = H[m], its age):
   dword by dword in PORDER, the prediction is candidate win[d] of the rule k0, the last value k1
   and the line through the last two k2; the residual is applied (dec) or made (enc), and the
   next winner is the first candidate closest to the value. k0 is
   - for the position, the last value plus this iteration's velocity, with the last step's drift
     carried: every record that survives the tick moved by its velocity, each branch with its own
     add (the guided one 0x49BB3A..0x49BB58; 0x49BB82, 0x49BC37, 0x49BCE3, 0x49BD12, 0x49BD50), and
     the paths without one mark the record done (0x499EB0 sets +0x69 bit 1), DISASSEMBLED;
   - for the velocity, the guided rule when the last image followed it, else the last value;
   - for +0x67 (dword 25's top byte, dword 26's low byte), the compaction's stamp: 0x49AE20
     writes each record's index there before it slides the survivors down (0x49AE5F,
     DISASSEMBLED), so a survivor's stamp is the slot it held in the last image, m;
   - else the last value.
   Each is a predictor: a wrong one costs size. */
static void pool_step(const RecCodec* c, unsigned m, uint32_t* u, uint32_t* res, unsigned char* win,
                      int dec)
{
    const uint32_t* L = c->pool + (size_t)m * PDW;
    const uint32_t* L2 = c->pool_l2 + (size_t)m * PDW;
    unsigned a = c->page[m], i, k, best;
    int hl = held(L);
    uint32_t g[3] = { 0, 0, 0 }, cand[3];
    for (i = 0; i < PDW; i++) {
        unsigned d = PORDER[i];
        uint32_t l1 = L[d], l2 = a >= 2 ? L2[d] : l1;
        cand[1] = l1;
        cand[2] = 2u * l1 - l2;
        if (d >= 1 && d <= 3) {
            cand[0] = l1 + u[d + 6] + (a >= 2 ? l1 - l2 - L[d + 6] : 0u);
        } else if (d >= 7 && d <= 9) {
            if (d == 7 && hl) guided(u, g);
            cand[0] = hl ? g[d - 7] : l1;
        } else if (d == 25) {
            cand[0] = (cand[2] & 0x00FFFFFFu) | (uint32_t)(m & 0xFF) << 24;
        } else if (d == 26) {
            cand[0] = (l1 & 0xFFFFFF00u) | (m >> 8 & 0xFF);
        } else {
            cand[0] = l1;
        }
        if (dec) u[d] = cand[win[d]] + res[d];
        else res[d] = u[d] - cand[win[d]];
        best = 0;
        for (k = 1; k < 3; k++)
            if (err_of(u[d], cand[k], 0xFFFFFFFFu) < err_of(u[d], cand[best], 0xFFFFFFFFu)) best = k;
        win[d] = (unsigned char)best;
    }
}

/* PS_ADV's row kk: the survivor step of old slot kk with no residual and a scratch copy of its
   winners, from the state before this iteration; made once an iteration (adv_done). */
static const uint32_t* adv_row(RecCodec* c, unsigned kk)
{
    uint32_t* u = c->adv + (size_t)kk * PDW;
    if (!c->adv_done[kk]) {
        uint32_t zero[PDW];
        unsigned char w[PDW];
        memset(zero, 0, sizeof zero);
        memcpy(w, c->pwin + (size_t)kk * PDW, PDW);
        pool_step(c, kk, u, zero, w, 1);
        c->adv_done[kk] = 1;
    }
    return u;
}

/* An EV row from a shot body b: the last birth of its weapon (lastnew), with what the 0x0D
   packet says of the shot written over it. The event's +0x01 is the start the build used, which
   0x49C740 writes to the record's position and tail (0x49C754..0x49C77B): the sender's three
   locals at +0x01 (0x49D7AD..0x49D81D, DISASSEMBLED), or for 0x49D9C0, whose packet carries the
   shooter's centre, the recorder's copy of its build's start (tagpu_patches.c, wpn_tx_fired).
   The sender puts the target point at +0x0D, the weapon's ID byte at +0x19 and the target's and
   shooter's unit indices at +0x1F and +0x21; the recorder masks the event (tagpu_recfmt.h RK_EV_SHOT) and puts the weapon
   index's high bits in +0x23's high nibble in the raised build. So: +0x00 the weapon ID (index
   + 1), the position +0x04 and the tail +0x10 the start, the target point +0x28, and the
   target +0x4E and attacker +0x52 as the image's unit IDs, which are the event's. In the
   stock-limits build that nibble is not weapon bits [INFERRED]: the row may name the wrong
   weapon, which costs size only. */
static void ev_row(const RecCodec* c, const unsigned char* b, uint32_t* row)
{
    unsigned w = b[0x19] | (unsigned)(b[0x23] >> 4) << 8, i;
    unsigned char r[PIMG];
    const uint32_t* base = c->lastnew + (size_t)(w + 1) * PDW;
    for (i = 0; i < PDW; i++) put32(r + 4 * i, base[i]);
    put32(r, w + 1);
    memcpy(r + 0x04, b + 0x01, 12);
    memcpy(r + 0x10, b + 0x01, 12);
    memcpy(r + 0x28, b + 0x0D, 12);
    put32(r + 0x4E, get16(b + 0x1F));
    put32(r + 0x52, get16(b + 0x21));
    for (i = 0; i < PDW; i++) row[i] = get32(r + 4 * i);
}

/* +0x67's stamp: a birth appended at the count holds slot c0 + i until the compaction writes
   its index there (every append is at the count: the ten appends, exe-reverse-engineering.md) */
static void stamp_row(uint32_t* r, unsigned s)
{
    r[25] = (r[25] & 0x00FFFFFFu) | (uint32_t)(s & 0xFF) << 24;
    r[26] = (r[26] & 0xFFFFFF00u) | (s >> 8 & 0xFF);
}

/* THE REFERENCE-CODED ROW of R (NULL: the zero row), stamped with s for a birth: every dword but
   the velocity is R's plus its residual; the velocity is then the guided rule of the row itself
   when R follows the rule, else R's, plus its residual. The row is made from `res` (dec) or `res`
   from the row (enc). */
static void ref_row(const uint32_t* R, int birth, unsigned s, uint32_t* res, uint32_t* row, int dec)
{
    uint32_t p[PDW], g[3];
    unsigned d;
    if (R) memcpy(p, R, PIMG);
    else memset(p, 0, PIMG);
    if (birth) stamp_row(p, s);
    for (d = 0; d < PDW; d++) {
        if (d >= 7 && d <= 9) continue;
        if (dec) row[d] = p[d] + res[d];
        else res[d] = row[d] - p[d];
    }
    if (held(p)) {
        guided(row, g);
        p[7] = g[0];
        p[8] = g[1];
        p[9] = g[2];
    }
    for (d = 7; d <= 9; d++) {
        if (dec) row[d] = p[d] + res[d];
        else res[d] = row[d] - p[d];
    }
}

/* A projectile record onto an image of the pool by the reader's rules (tagpu_recreader.c,
   RK_PROJ_HEAD..RK_PROJ_DELTA, but for the order of the file, which decode before write makes
   irrelevant: the bytes decoded are the bytes encoded). 0 where the reader refuses. */
static int pool_apply(const RecCodec* c, uint32_t* img, unsigned* count, unsigned* hw, unsigned kind,
                      const unsigned char* b, unsigned len)
{
    unsigned id, i, n = 0, k;
    uint32_t bits;
    if (kind == RK_PROJ_HEAD) {
        uint32_t v0, v1;
        if (len != 8) return 0;
        v0 = get32(b);
        v1 = get32(b + 4);
        if (v0 > v1 || v1 > c->nproj) return 0;
        /* records past the extent are zero in every copy: a shrink gives them up */
        if (v1 < *hw) memset(img + (size_t)v1 * PDW, 0, (size_t)(*hw - v1) * PIMG);
        *count = v0;
        *hw = v1;
        return 1;
    }
    if (len < 2) return 0;
    id = get16(b);
    if (kind == RK_PROJ) {
        if (len != 4 + PIMG || b[2] || b[3] || id >= *hw) return 0;
        for (i = 0; i < PDW; i++) img[(size_t)id * PDW + i] = get32(b + 4 + 4 * i);
        return 1;
    }
    if (len < REC_PROJ_DELTA_HEAD || id >= *hw) return 0;
    bits = get32(b + 2);
    if (bits >> PDW) return 0;
    for (i = 0; i < PDW; i++) n += bits >> i & 1;
    if (len != REC_PROJ_DELTA_HEAD + 4 * n) return 0;
    for (i = 0, k = 0; i < PDW; i++)
        if (bits >> i & 1) img[(size_t)id * PDW + i] = get32(b + REC_PROJ_DELTA_HEAD + 4 * k++);
    return 1;
}

/* The body of an RK_PROJ (whole) or RK_PROJ_DELTA (the dwords that differ from `old`) for slot
   j with image `row`, as the capture makes it (cap_proj): its length. */
static unsigned pool_body(unsigned kind, unsigned j, const uint32_t* old, const uint32_t* row,
                          unsigned char* out)
{
    unsigned d, n = 0;
    uint32_t bits = 0;
    out[0] = (unsigned char)j;
    out[1] = (unsigned char)(j >> 8);
    if (kind == RK_PROJ) {
        out[2] = out[3] = 0;
        for (d = 0; d < PDW; d++) put32(out + 4 + 4 * d, row[d]);
        return 4 + PIMG;
    }
    for (d = 0; d < PDW; d++)
        if (row[d] != old[d]) {
            bits |= (uint32_t)1 << d;
            put32(out + REC_PROJ_DELTA_HEAD + 4 * n++, row[d]);
        }
    put32(out + 2, bits);
    out[6] = out[7] = 0;
    return REC_PROJ_DELTA_HEAD + 4 * n;
}

/* The pool frame restarts from a world's pool (tagpu_reccodec.h): the copy and its count and
   extent from the world, every other piece of state cleared -- no history, the rule the first
   winner, no last birth, no shot. 0: a count past the extent or an extent past the pool. */
static int pool_restart(RecCodec* c, const unsigned char* pool, unsigned count, unsigned hw)
{
    unsigned i, n = c->nproj;
    if (count > hw || hw > n) return 0;
    if (pool != (const unsigned char*)c->pool)
        for (i = 0; i < hw * PDW; i++) c->pool[i] = get32(pool + 4 * (size_t)i);
    memset(c->pool + (size_t)hw * PDW, 0, (size_t)(n - hw) * PIMG);
    memset(c->pool_n, 0, (size_t)n * PIMG);
    memset(c->pwin, 0, (size_t)n * PDW);
    memset(c->page, 0, n);
    memset(c->page, 1, count);
    memset(c->lastnew, 0, (size_t)(RECC_WEAPON_IDS + 1) * PIMG);
    c->pcount = count;
    c->phw = hw;
    c->nshots = 0;
    return 1;
}

/* THE CANONICAL TEST. The capture sends, after an optional RK_PROJ_HEAD, one record for each
   slot below hw whose image changed, ascending, whole or as the delta of exactly the dwords that
   changed (cap_proj), and records past the extent are zero in every copy, so the pool before and
   after an iteration fix every body but each record's kind, which RCS_KINDS keeps. An iteration
   of another form -- a writer's correction naming a slot twice, a second head, an extent that
   shrinks -- is declined before anything of it is applied: 0. Otherwise the iteration's pool is
   in pool_n, its count and extent in ncount and nhw, and its changed slots in pchg. Work: the
   records times 27 dwords, and the changed scan, nhw * 27. */
static int pool_check(RecCodec* c, const unsigned char* pl, size_t at, size_t end)
{
    unsigned count = c->pcount, hw = c->phw, heads = 0, nslot = 0, j, q;
    size_t p;
    unsigned char body[POOL_BODY_MAX];
    /* pool_n is zero from phw on: it is the pool of the iteration before, or a restart's zeros */
    memcpy(c->pool_n, c->pool, (size_t)c->phw * PIMG);
    for (p = at; p < end; p += (4 + (size_t)get16(pl + p + 2) + 3) & ~(size_t)3) {
        unsigned kind = get16(pl + p), len = get16(pl + p + 2);
        if (kind != RK_PROJ_HEAD && kind != RK_PROJ && kind != RK_PROJ_DELTA) continue;
        if (!pool_apply(c, c->pool_n, &count, &hw, kind, pl + p + 4, len)) return 0;
        if (kind == RK_PROJ_HEAD && (++heads > 1 || hw < c->phw)) return 0;
        if (kind != RK_PROJ_HEAD) nslot++;
    }
    c->nchg = 0;
    for (j = 0; j < hw; j++)
        if (memcmp(c->pool_n + (size_t)j * PDW, c->pool + (size_t)j * PDW, PIMG)) c->pchg[c->nchg++] = j;
    if (nslot != c->nchg) return 0;
    for (p = at, q = 0; p < end; p += (4 + (size_t)get16(pl + p + 2) + 3) & ~(size_t)3) {
        unsigned kind = get16(pl + p), len = get16(pl + p + 2);
        if (kind != RK_PROJ && kind != RK_PROJ_DELTA) continue;
        j = c->pchg[q++];
        if (pool_body(kind, j, c->pool + (size_t)j * PDW, c->pool_n + (size_t)j * PDW, body) != len ||
            memcmp(body, pl + p + 4, len))
            return 0;
    }
    c->ncount = count;
    c->nhw = hw;
    return 1;
}

/* the search's keys: the weapon ID (+0x00) when it is one, else 0; the attacker (+0x52) */
static unsigned wkey(const uint32_t* r) { return r[0] >= 1 && r[0] <= RECC_WEAPON_IDS ? r[0] : 0; }
static unsigned shkey(const uint32_t* r) { return r[20] >> 16; }

static unsigned chains_slot(const Chains* x, uint32_t key, unsigned i)
{
    return (((key * 2654435761u) >> (32 - x->tbits)) + i) & (x->tsize - 1);
}

static void chains_reset(Chains* x)
{
    unsigned i;
    for (i = 0; i <= RECC_WEAPON_IDS; i++) x->headw[i] = -1;
    for (i = 0; i < x->nused; i++) x->top[x->used[i]] = -1;
    x->nused = 0;
}

/* Row r, the section's latest: a row whose (weapon, shooter) key finds no slot within
   RECC_PROBE_MAX stays out of the table and in its weapon chain (size only). */
static void chains_add(Chains* x, unsigned r, const uint32_t* row)
{
    unsigned w = wkey(row), i;
    uint32_t key = (uint32_t)w << 16 | shkey(row);
    x->prevw[r] = x->headw[w];
    x->headw[w] = (int32_t)r;
    x->prevws[r] = -1;
    for (i = 0; i < RECC_PROBE_MAX; i++) {
        unsigned t = chains_slot(x, key, i);
        if (x->top[t] < 0) {
            x->key[t] = key;
            x->top[t] = (int32_t)r;
            x->used[x->nused++] = t;
            return;
        }
        if (x->key[t] == key) {
            x->prevws[r] = x->top[t];
            x->top[t] = (int32_t)r;
            return;
        }
    }
}

/* Up to RECC_CAND rows of weapon key w: those of shooter sh first, then the rest, each group the
   latest first -- RECC_CAND + 3 chain steps at most. */
static unsigned chains_cands(const Chains* x, unsigned w, unsigned sh, int32_t* out)
{
    uint32_t key = (uint32_t)w << 16 | sh;
    unsigned n = 0, t, i, k;
    int32_t r;
    for (i = 0; i < RECC_PROBE_MAX; i++) {
        unsigned s = chains_slot(x, key, i);
        if (x->top[s] < 0) break;
        if (x->key[s] == key) {
            for (r = x->top[s]; r >= 0 && n < RECC_CAND; r = x->prevws[r]) out[n++] = r;
            break;
        }
    }
    t = n;
    for (r = x->headw[w]; r >= 0 && n < RECC_CAND; r = x->prevw[r]) {
        for (k = 0; k < t && out[k] != r; k++) ;
        if (k == t) out[n++] = r;
    }
    return n;
}

/* a section's row kk (in range) */
static const uint32_t* sec_row(RecCodec* c, unsigned sec, unsigned kk)
{
    switch (sec) {
    case PS_OUT: return c->pool_n + (size_t)kk * PDW;
    case PS_OLD: return c->pool + (size_t)kk * PDW;
    case PS_ADV: return adv_row(c, kk);
    default:     return c->ev + (size_t)kk * PDW;
    }
}

/* The best reference so far: the lowest cost, ties to the earlier section, then to the earlier
   candidate. The cost is the residuals' varint bytes. */
typedef struct Best { unsigned cost, sec; int64_t kk; int any; } Best;

static void consider(RecCodec* c, Best* b, unsigned sec, unsigned kk, int birth, unsigned s,
                     uint32_t* row)
{
    uint32_t res[PDW];
    unsigned d, cost = 0;
    ref_row(sec_row(c, sec, kk), birth, s, res, row, 0);
    for (d = 0; d < PDW; d++) cost += leblen(zz32(res[d]));
    if (!b->any || cost < b->cost) {
        b->any = 1;
        b->cost = cost;
        b->sec = sec;
        b->kk = kk;
    }
}

/* A reference's code (tagpu_recfmt.h, RCS_PROJ): 0 the zero row, else 1 + (zigzag(kk - anchor)
   << 2 | section). */
static uint32_t ref_code(const Best* b, const int64_t anchor[4])
{
    return b->any ? 1 + (zz64(b->kk - anchor[b->sec]) << 2 | b->sec) : 0;
}

/* The reference of birth k (stamp s): per section the RECC_CAND rows of its weapon, the shooter
   first, then the latest -- the research's "same weapon, same shooter first, nearest the
   anchor" for anchors at the sections' ends -- PS_ADV taking PS_OLD's rows; with no row of its
   weapon anywhere, the last RECC_CAND rows of each section, ordered the same. */
static uint32_t pool_birth_ref(RecCodec* c, unsigned k, unsigned s, uint32_t* row, unsigned c0,
                               unsigned ne, uint32_t* res, Best* b)
{
    static const unsigned SEC[3] = { PS_OUT, PS_OLD, PS_EV };
    int32_t cand[3][RECC_CAND];
    unsigned n[3], len[3] = { k, c0, ne }, w = wkey(row), sh = shkey(row), i, q, t;
    int64_t anchor[4];
    anchor[PS_OUT] = (int64_t)k - 1;
    anchor[PS_OLD] = anchor[PS_ADV] = (int64_t)c0 - 1;
    anchor[PS_EV] = c->evnext < ne ? (int64_t)c->evnext : (int64_t)ne - 1;
    for (i = 0; i < 3; i++) n[i] = c->ch[i].headw[w] >= 0 ? chains_cands(&c->ch[i], w, sh, cand[i]) : 0;
    if (!n[0] && !n[1] && !n[2])
        for (i = 0; i < 3; i++) {
            const uint32_t* base = SEC[i] == PS_OUT ? c->pool_n : SEC[i] == PS_OLD ? c->pool : c->ev;
            unsigned lo = len[i] > RECC_CAND ? len[i] - RECC_CAND : 0, pass;
            for (pass = 0; pass < 2; pass++)
                for (t = len[i]; t-- > lo;)
                    if ((shkey(base + (size_t)t * PDW) == sh) == !pass) cand[i][n[i]++] = (int32_t)t;
        }
    memset(b, 0, sizeof *b);
    for (q = 0; q < 4; q++) {
        unsigned from = q == PS_OUT ? 0 : q == PS_EV ? 2 : 1;
        for (t = 0; t < n[from]; t++) consider(c, b, q, (unsigned)cand[from][t], 1, s, row);
    }
    ref_row(b->any ? sec_row(c, b->sec, (unsigned)b->kk) : NULL, 1, s, res, row, 0);
    if (b->any && b->sec == PS_EV) c->evnext = (unsigned)b->kk + 1;
    return ref_code(b, anchor);
}

/* The reference of tail slot j: in each section the rows within RECC_TAIL_WINDOW of its anchor
   with its weapon -- and in PS_OLD slot j itself whatever its weapon, but no other stale slot --
   the shooter first, then the nearest, then the lower; RECC_CAND of them. With none in any
   section, the same windows without the weapon test. */
static uint32_t pool_tail_ref(RecCodec* c, unsigned j, uint32_t* row, const int64_t anchor[4],
                              const unsigned len[4], unsigned c0, uint32_t* res)
{
    int32_t cand[4][2 * RECC_TAIL_WINDOW + 1];
    unsigned n[4], w = wkey(row), sh = shkey(row), q, t, any = 0, pass;
    Best b;
    for (pass = 0; pass < 2 && !any; pass++)
        for (q = 0; q < 4; q++) {
            int64_t r, lo = anchor[q] - RECC_TAIL_WINDOW, hi = anchor[q] + RECC_TAIL_WINDOW;
            n[q] = 0;
            if (lo < 0) lo = 0;
            if (hi > (int64_t)len[q] - 1) hi = (int64_t)len[q] - 1;
            for (r = lo; r <= hi; r++) {
                int own = q == PS_OLD && r == j;
                if (q == PS_OLD && r >= c0 && !own) continue;
                if (pass == 0 && !own && wkey(sec_row(c, q, (unsigned)r)) != w) continue;
                cand[q][n[q]++] = (int32_t)r;
            }
            any |= n[q];
        }
    memset(&b, 0, sizeof b);
    for (q = 0; q < 4; q++) {
        /* the first RECC_CAND by (another shooter, distance, row): an insertion sort of <= 33 */
        unsigned i, k;
        for (i = 1; i < n[q]; i++) {
            int32_t r = cand[q][i];
            int64_t key = (int64_t)(shkey(sec_row(c, q, (unsigned)r)) != sh) << 32 |
                          (r > anchor[q] ? r - anchor[q] : anchor[q] - r) << 16 | r;
            for (k = i; k > 0; k--) {
                int32_t o = cand[q][k - 1];
                int64_t ko = (int64_t)(shkey(sec_row(c, q, (unsigned)o)) != sh) << 32 |
                             (o > anchor[q] ? o - anchor[q] : anchor[q] - o) << 16 | o;
                if (ko <= key) break;
                cand[q][k] = o;
            }
            cand[q][k] = r;
        }
        for (t = 0; t < n[q] && t < RECC_CAND; t++) consider(c, &b, q, (unsigned)cand[q][t], 0, 0, row);
    }
    ref_row(b.any ? sec_row(c, b.sec, (unsigned)b.kk) : NULL, 0, 0, res, row, 0);
    return ref_code(&b, anchor);
}

static void pool_put(RecCodec* c, RecBytes* b, const uint32_t* res)
{
    unsigned d;
    uint32_t z[PDW];
    if (c->over) return;
    for (d = 0; d < PDW; d++) z[d] = zz32(res[d]);
    rb_put(b, z, sizeof z);
}

/* One iteration that passed pool_check, coded into the block's buffers, then committed: the
   state takes it whatever the buffers do (past RECC_FRAME_CAP the block is declined after its
   iterations, so the state always ends as the payload leaves the pool).
   THE MATCHER pairs the new records with the old: a new record whose stamp names an old slot
   after the last pairing and is similar to it -- the same +0x00 and RECC_MATCH_EQUAL dwords --
   is that record, else the first similar slot of the next RECC_MATCH_WINDOW; the survivors are
   the new records up to the first with no pairing. Policy: any pairing it makes is valid frame
   input (the survivors a prefix, their old slots ascending), and a wrong one costs size.
   BOUNDED BY CONSTRUCTION, per iteration (N = nproj): the matcher 9 similarity tests a record;
   a survivor one step of 27 dwords; a birth 3 lookups of RECC_PROBE_MAX probes and RECC_CAND + 3
   chain steps, then at most 4 x RECC_CAND candidates of 27 dwords; a tail slot 4 windows of
   2 RECC_TAIL_WINDOW + 1 rows and 4 x RECC_CAND candidates; PS_ADV at most c0 steps. Births and
   tail slots together are at most N. */
static void pool_encode(RecCodec* c, const unsigned char* pl, size_t at, size_t end)
{
    const unsigned N = c->nproj, c0 = c->pcount, h0 = c->phw, c1 = c->ncount, h1 = c->nhw;
    unsigned ns, nt = 0, j, i, prev, d;
    int64_t last = -1;
    uint32_t res[PDW];
    size_t p;
    /* the matcher */
    for (j = 0; j < c1; j++) {
        const uint32_t* u = c->pool_n + (size_t)j * PDW;
        unsigned s = u[25] >> 24 | (u[26] & 0xFF) << 8, s2, hi;
        int64_t hit = -1;
        if ((int64_t)s > last && s < c0 && u[0] == c->pool[(size_t)s * PDW]) {
            unsigned eq = 0;
            for (d = 0; d < PDW; d++) eq += u[d] == c->pool[(size_t)s * PDW + d];
            if (eq >= RECC_MATCH_EQUAL) hit = s;
        }
        hi = (unsigned)(last + 1 + RECC_MATCH_WINDOW < c0 ? last + 1 + RECC_MATCH_WINDOW : c0);
        for (s2 = (unsigned)(last + 1); hit < 0 && s2 < hi; s2++) {
            unsigned eq = 0;
            if (u[0] != c->pool[(size_t)s2 * PDW]) continue;
            for (d = 0; d < PDW; d++) eq += u[d] == c->pool[(size_t)s2 * PDW + d];
            if (eq >= RECC_MATCH_EQUAL) hit = s2;
        }
        if (hit < 0) break;
        c->pm[j] = (int32_t)hit;
        last = hit;
    }
    ns = j;
    for (i = 0; i < c0; i++) c->pinv[i] = -1;
    for (j = 0; j < ns; j++) c->pinv[c->pm[j]] = (int32_t)j;
    for (i = 0; i < c->nchg; i++)
        if (c->pchg[i] >= c1) c->ptl[nt++] = c->pchg[i];
    /* the control values */
    if (!c->over) {
        rb_varint(&c->pctl, zz64((int64_t)c1 - c0));
        rb_varint(&c->pctl, h1 - h0);
        rb_varint(&c->pctl, c0 - ns);
        for (i = 0, prev = 0; i < c0; i++)
            if (c->pinv[i] < 0) {
                rb_varint(&c->pctl, i - prev);
                prev = i + 1;
            }
        rb_varint(&c->pctl, nt);
        for (i = 0, prev = c1; i < nt; i++) {
            rb_varint(&c->pctl, c->ptl[i] - prev);
            prev = c->ptl[i] + 1;
        }
    }
    /* the survivors */
    for (j = 0; j < ns; j++) {
        unsigned m = (unsigned)c->pm[j];
        memcpy(c->pwin_n + (size_t)j * PDW, c->pwin + (size_t)m * PDW, PDW);
        pool_step(c, m, c->pool_n + (size_t)j * PDW, res, c->pwin_n + (size_t)j * PDW, 0);
        pool_put(c, &c->psurv, res);
        memcpy(c->pool_l2n + (size_t)j * PDW, c->pool + (size_t)m * PDW, PIMG);
        c->page_n[j] = (unsigned char)(c->page[m] >= 2 ? 2 : c->page[m] + 1);
    }
    /* the reference sections, from the state before the births */
    for (i = 0; i < c->nshots; i++) ev_row(c, c->shots + (size_t)i * SHOT, c->ev + (size_t)i * PDW);
    memset(c->adv_done, 0, c0);
    for (i = 0; i < 3; i++) chains_reset(&c->ch[i]);
    for (j = 0; j < ns; j++) chains_add(&c->ch[0], j, c->pool_n + (size_t)j * PDW);
    for (j = 0; j < c0; j++) chains_add(&c->ch[1], j, c->pool + (size_t)j * PDW);
    for (j = 0; j < c->nshots; j++) chains_add(&c->ch[2], j, c->ev + (size_t)j * PDW);
    /* the births */
    c->evnext = 0;
    for (j = ns; j < c1; j++) {
        uint32_t* row = c->pool_n + (size_t)j * PDW;
        Best b;
        uint32_t code = pool_birth_ref(c, j, (c0 + j - ns) & 0xFFFF, row, c0, c->nshots, res, &b);
        if (!c->over) rb_varint(&c->pref, code);
        pool_put(c, &c->pbirth, res);
        if (row[0] >= 1 && row[0] <= RECC_WEAPON_IDS) memcpy(c->lastnew + (size_t)row[0] * PDW, row, PIMG);
        chains_add(&c->ch[0], j, row);
        memset(c->pwin_n + (size_t)j * PDW, 0, PDW);
        c->page_n[j] = 1;
    }
    /* the tail */
    for (i = 0; i < nt; i++) {
        unsigned tj = c->ptl[i], len[4] = { c1, h0, c0, c->nshots };
        int64_t anchor[4];
        anchor[PS_OUT] = tj < c0 ? (c->pinv[tj] >= 0 ? c->pinv[tj] : (tj < c1 ? tj : (int64_t)c1 - 1))
                                 : (ns + (tj - c0) < c1 ? ns + (tj - c0) : (int64_t)c1 - 1);
        anchor[PS_OLD] = tj < h0 ? tj : (int64_t)h0 - 1;
        anchor[PS_ADV] = tj < c0 ? tj : (int64_t)c0 - 1;
        anchor[PS_EV] = (int64_t)c->nshots - 1;
        {
            uint32_t code = pool_tail_ref(c, tj, c->pool_n + (size_t)tj * PDW, anchor, len, c0, res);
            if (!c->over) rb_varint(&c->pref, code);
        }
        pool_put(c, &c->ptail, res);
    }
    if (c->pctl.n + c->pref.n + c->psurv.n + c->pbirth.n + c->ptail.n > RECC_FRAME_CAP) c->over = 1;
    /* the commit */
    memset(c->page_n + c1, 0, N - c1);
    {
        uint32_t* t = c->pool; c->pool = c->pool_n; c->pool_n = t;
        t = c->pool_l2; c->pool_l2 = c->pool_l2n; c->pool_l2n = t;
    }
    {
        unsigned char* t = c->pwin; c->pwin = c->pwin_n; c->pwin_n = t;
        t = c->page; c->page = c->page_n; c->page_n = t;
    }
    c->pcount = c1;
    c->phw = h1;
    c->nshots = 0;
    for (p = at; p < end; p += (4 + (size_t)get16(pl + p + 2) + 3) & ~(size_t)3)
        if (get16(pl + p) == RK_EV_SHOT && get16(pl + p + 2) == SHOT && c->nshots < RECC_SHOTS_MAX)
            memcpy(c->shots + (size_t)c->nshots++ * SHOT, pl + p + 4, SHOT);
}

/* RCS_PROJ from the block's buffers (tagpu_recfmt.h): 0 when it passes RECC_FRAME_CAP or memory */
static int pool_write(RecCodec* c, unsigned iterations)
{
    RecBytes* o = &c->sec[RCS_PROJ];
    const RecBytes* part[3] = { &c->psurv, &c->pbirth, &c->ptail };
    unsigned i, d;
    size_t r, rows;
    o->n = 0;
    o->oom = 0;
    rb_varint(o, iterations);
    rb_put(o, c->pctl.p, c->pctl.n);
    rb_put(o, c->pref.p, c->pref.n);
    for (i = 0; i < 3; i++) {
        const uint32_t* z = (const uint32_t*)part[i]->p;
        rows = part[i]->n / PIMG;
        for (d = 0; d < PDW; d++) {
            unsigned dw = i ? d : PORDER[d];
            for (r = 0; r < rows; r++) rb_varint(o, z[r * PDW + dw]);
        }
    }
    return !o->oom && o->n <= RECC_FRAME_CAP;
}

/* ---- the encoder ---------------------------------------------------------------------------- */

/* One record onto the unit copy by the writer's own rules (tagpu_recwriter.c `apply`): an
   RK_UNIT, an RK_UNIT_GONE, or an RK_UNIT_DELTA of a live slot whose bitmap and length agree. */
static void plain_record(RecCodec* c, unsigned kind, const unsigned char* b, unsigned len)
{
    unsigned s = len >= 2 ? get16(b) : 0, i, k;
    if (s < 1 || s >= c->nslots) return;
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

/* On a decline, the copies take records [at, n): the units by plain_record, the projectile
   records the pool frame did not carry by the reader's rules, and each record frame's kinds onto
   that frame's part of the world (RecFrameOps.plain), so the copies -- the units, the pool and
   the frames' world -- always end as the payload leaves them. A delta the unit frame did carry is
   U already, which applying it again leaves; an iteration the pool frame carried is committed
   already (pool_encode). `at` is an iteration's start (reccodec_encode's two calls), so `events`
   starts as the reader's rule has it there. */
static void plain_apply(RecCodec* c, const unsigned char* pl, size_t at, size_t n)
{
    int events = 0;
    while (n - at >= 4) {
        unsigned kind = get16(pl + at), len = get16(pl + at + 2), sc;
        size_t rl = ((size_t)4 + len + 3) & ~(size_t)3;
        const unsigned char* b = pl + at + 4;
        if (rl > n - at) break;
        if (kind == RK_TICK) events = 0;
        if (kind == RK_PROJ_HEAD || kind == RK_PROJ || kind == RK_PROJ_DELTA)
            pool_apply(c, c->pool, &c->pcount, &c->phw, kind, b, len);
        plain_record(c, kind, b, len);
        sc = rec_section_of(kind);
        if (c->fix[sc] >= 0 && FRAMES[c->fix[sc]]->plain)
            FRAMES[c->fix[sc]]->plain(c->frame[c->fix[sc]], kind, b, len, events);
        if (sc == RCS_EVENTS) events = 1;
        at += rl;
    }
}

/* ---- THE KINDS -------------------------------------------------------------------------------- */

#define KM_TICKPAIR 0u          /* the kinds' marker of an empty feature tick (tagpu_recfmt.h)  */
#define KM_FEATURES 0xFFFFu     /* the kinds' marker of a predicted RK_FEATURES                  */
#define RUN_DERIVED  1u         /* a decoder's run: its count is its group's total less the rest */
#define RUN_IMPLICIT 2u         /* a decoder's run: a marker's record, which no frame codes      */

/* a kind's group, whose first run of an iteration's pattern has no count: 0 RK_TICK, 1
   RK_UNIT_DELTA, 2 RK_PROJ and RK_PROJ_DELTA; -1 none */
static int group_of(uint32_t kind)
{
    if (kind == RK_TICK) return 0;
    if (kind == RK_UNIT_DELTA) return 1;
    if (kind == RK_PROJ || kind == RK_PROJ_DELTA) return 2;
    return -1;
}

/* The pattern list: entry i moves to the front */
static void kmru_front(RecCodec* c, unsigned i)
{
    RecBytes t = c->kmru[i];
    memmove(&c->kmru[1], &c->kmru[0], (size_t)i * sizeof t);
    c->kmru[0] = t;
}

/* a new pattern at the front, the list's last dropped when full; kbad latched without memory */
static void kmru_insert(RecCodec* c, const unsigned char* p, size_t n)
{
    unsigned i = c->nkmru < RECC_KINDS_MRU ? c->nkmru++ : RECC_KINDS_MRU - 1;
    c->kmru[i].n = 0;
    rb_put(&c->kmru[i], p, n);
    if (c->kmru[i].oom) c->kbad = 1;
    kmru_front(c, i);
}

/* the encoder's iteration gains a record of `kind`, or a marker: a run of the kind before it
   grows, a marker stands alone */
static void ktok_add(RecCodec* c, uint32_t kind)
{
    size_t n = c->ktok.n / 8;
    uint32_t* t = (uint32_t*)c->ktok.p;
    if (n && kind != KM_TICKPAIR && kind != KM_FEATURES && t[2 * (n - 1)] == kind) {
        t[2 * (n - 1) + 1]++;
        return;
    }
    rb_u32(&c->ktok, kind);
    rb_u32(&c->ktok, 1);
}

/* The iteration's pattern (tagpu_recfmt.h, RCS_KINDS) from its runs, by reference when the list
   holds it; a pattern past RECC_KINDS_MAX or an allocation that failed latches kbad, which
   declines the block. */
static void kinds_write(RecCodec* c)
{
    const uint32_t* t = (const uint32_t*)c->ktok.p;
    size_t n = c->ktok.n / 8, i;
    unsigned seen = 0, m;
    RecBytes* o = &c->sec[RCS_KINDS];
    c->kpat.n = 0;
    c->kpat.oom = 0;
    for (i = 0; i < n; i++) {
        uint32_t kind = t[2 * i];
        int g = group_of(kind);
        rb_varint(&c->kpat, kind);
        if (kind == KM_TICKPAIR || kind == KM_FEATURES) continue;
        if (g >= 0 && !(seen >> g & 1)) {
            seen |= 1u << g;
            continue;
        }
        rb_varint(&c->kpat, t[2 * i + 1]);
    }
    if (c->ktok.oom || c->kpat.oom || c->kpat.n > RECC_KINDS_MAX) {
        c->kbad = 1;
        return;
    }
    for (m = 0; m < c->nkmru; m++)
        if (c->kmru[m].n == c->kpat.n && !memcmp(c->kmru[m].p, c->kpat.p, c->kpat.n)) break;
    if (m < c->nkmru) {
        rb_u8(o, m + 1);
        kmru_front(c, m);
        return;
    }
    rb_u8(o, 0);
    rb_varint(o, (uint32_t)c->kpat.n);
    rb_put(o, c->kpat.p, c->kpat.n);
    kmru_insert(c, c->kpat.p, c->kpat.n);
}

/* an RK_EV_FEATURE body of the feature tick's root with no arguments, `e` the record after it:
   the pair is an empty feature tick when that record is the tick's end */
static int tick_pair(unsigned kind, const unsigned char* b, unsigned len, const unsigned char* e,
                     size_t left)
{
    return kind == RK_EV_FEATURE && len == 8 && b[0] == FR_TICK && left >= 12 &&
           get16(e) == RK_EV_FEATURE_END && get16(e + 2) == 8 && e[4] == FR_TICK;
}

/* The record frames' walk of iteration `it` [at, end), after the unit and pool frames: each
   frame's enc_iter, then the records in order -- RK_UNIT and RK_UNIT_GONE onto the unit copy,
   each frame kind to its frame, the events flag set past an event -- then each enc_iter_end, and
   the iteration's pattern. An empty feature tick equal to the run's last explicit one and an
   RK_FEATURES equal to the state frame's prediction after an explicit one are markers, which no
   frame codes; the decoder's pass B keeps the same state in the same order. */
static void frames_walk(RecCodec* c, const unsigned char* pl, size_t at, size_t end, unsigned it,
                        uint32_t gt)
{
    RecFrameAt a;
    size_t p, rl;
    unsigned i;
    void* state = c->frame[c->fix[RCS_STATE]];
    a.it = it;
    a.gt = gt;
    a.events = 0;
    a.nslots = c->nslots;
    a.unit = c->unit;
    a.live = c->live;
    c->ktok.n = 0;
    c->ktok.oom = 0;
    for (i = 0; i < RECC_FRAMES; i++) FRAMES[i]->enc_iter(c->frame[i], &a);
    for (p = at; p < end; p += rl) {
        unsigned kind = get16(pl + p), len = get16(pl + p + 2), sc = rec_section_of(kind);
        const unsigned char* b = pl + p + 4;
        rl = ((size_t)4 + len + 3) & ~(size_t)3;
        if (tick_pair(kind, b, len, pl + p + rl, end - p - rl)) {
            if (c->tpaired && !memcmp(c->tpair, b, 8) && !memcmp(c->tpair + 8, pl + p + rl + 4, 8)) {
                ktok_add(c, KM_TICKPAIR);
                a.events = 1;
                rl += 12;
                continue;
            }
            memcpy(c->tpair, b, 8);
            memcpy(c->tpair + 8, pl + p + rl + 4, 8);
            c->tpaired = 1;
        } else if (kind == RK_FEATURES && len == 16 && c->featseen) {
            unsigned char pred[16];
            recframe_state_predicted(state, pred);
            if (!memcmp(pred, b, 16)) {
                recframe_state_take(state, b);
                ktok_add(c, KM_FEATURES);
                continue;
            }
        }
        if (kind == RK_FEATURES) c->featseen = 1;
        ktok_add(c, kind);
        if (kind == RK_UNIT || kind == RK_UNIT_GONE) plain_record(c, kind, b, len);
        else if (c->fix[sc] >= 0) FRAMES[c->fix[sc]]->enc_rec(c->frame[c->fix[sc]], &a, kind, b, len);
        if (sc == RCS_EVENTS) a.events = 1;
    }
    for (i = 0; i < RECC_FRAMES; i++) FRAMES[i]->enc_iter_end(c->frame[i], &a);
    kinds_write(c);
}

/* each record frame's section: 0 when one latched or passed its bounds */
static int frames_write(RecCodec* c)
{
    unsigned i;
    for (i = 0; i < RECC_FRAMES; i++)
        if (!FRAMES[i]->enc_write(c->frame[i], &c->sec[FRAMES[i]->section])) return 0;
    return 1;
}

/* Every record checked, and the bodies of RCS_OTHER's kinds into it but for the RK_TICK of every
   iteration after the first, whose GameTime must then be the first's + its index (the frames and
   the kinds make the rest). 0: the codec does not carry this payload. */
static int enc_prepass(RecCodec* c, const unsigned char* pl, size_t n, unsigned* ticks)
{
    size_t at = 0;
    unsigned i, nt = 0;
    uint32_t gt0 = 0;
    for (i = 0; i < RCS_COUNT; i++) c->sec[i].n = 0, c->sec[i].oom = 0;
    if (n < 4 || n % 4 || n > RECC_PAYLOAD_MAX) return 0;
    while (at < n) {
        unsigned kind = get16(pl + at), len = get16(pl + at + 2);
        size_t rl = ((size_t)4 + len + 3) & ~(size_t)3, k;
        const unsigned char* b = pl + at + 4;
        if (!kind || kind == KM_FEATURES || rl > n - at) return 0;
        for (k = 4 + len; k < rl; k++)
            if (pl[at + k]) return 0;
        if (!at && kind != RK_TICK) return 0;
        if (kind == RK_TICK && len != 4) return 0;
        if ((kind == RK_UNIT && len != 4 + IMG) || (kind == RK_UNIT_GONE && len != 4)) return 0;
        if ((kind == RK_UNIT || kind == RK_UNIT_GONE) && (get16(b) < 1 || get16(b) >= c->nslots)) return 0;
        if (kind == RK_UNIT_DELTA && len < REC_UNIT_DELTA_HEAD) return 0;
        if (kind == RK_TICK) {
            if (!nt) gt0 = get32(b);
            else if (get32(b) != gt0 + nt) return 0;
            nt++;
        }
        if (rec_section_of(kind) == RCS_OTHER && (kind != RK_TICK || nt == 1)) {
            rb_varint(&c->sec[RCS_OTHER], len);
            rb_put(&c->sec[RCS_OTHER], b, len);
        }
        at += rl;
    }
    for (i = 0; i < RCS_COUNT; i++)
        if (c->sec[i].oom) return 0;
    *ticks = nt;
    return nt >= 1 && nt <= RECC_TICKS_MAX;
}

/* one iteration [at, end) of the payload, every frame; 0: declined, nothing of it applied yet */
static int enc_iteration(RecCodec* c, const unsigned char* pl, size_t at, size_t end, unsigned it)
{
    size_t p, nd, k;
    uint32_t gt = get32(pl + at + 4);
    unsigned prev = 0, s, i, nc, wprev, nr = 0, nw = 0;
    int64_t rprev;
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
    if (!pool_check(c, pl, at, end)) goto decline;
    memset(&x, 0, sizeof x);
    x.c = c;
    x.gt = gt;
    k = 0;
    c->jr.n = c->jw.n = 0;
    for (s = 1, nc = 0, rprev = -1, wprev = 0; s < c->nslots; s++) {
        const unsigned char* d = NULL;
        unsigned j, nb;
        int carried = c->jprev[s], cand;
        c->jprev[s] = 0;
        if (!c->live[s] || c->touched[s]) continue;
        if (k < nd && get16(pl + ((const uint32_t*)c->dl.p)[k] + 4) == s)
            d = pl + ((const uint32_t*)c->dl.p)[k++] + 4;
        cand = !c->rest[s];
        if (!d && !cand) continue;
        x.l1 = (const unsigned char*)(c->unit + (size_t)s * NDW);
        x.l2 = (const unsigned char*)(c->l2 + (size_t)s * NDW);
        x.l3 = c->l3 + (size_t)s * L3B;
        x.win = c->win + (size_t)s * RECC_LANES_MAX;
        x.age = c->age[s];
        x.s = s;
        memcpy(x.u, x.l1, IMG);
        if (d)
            for (i = 0, nb = 0; i < NDW; i++)
                if (bit_of(d + 2, i)) memcpy(x.u + 4 * i, d + REC_UNIT_DELTA_HEAD + 4 * nb++, 4);
        memset(x.rmask, 0, sizeof x.rmask);
        x.any = 0;
        frame_slot(&x);
        if (!cand && !x.any) c->jbad = 1;     /* a woken slot always lists: the block declines */
        if (!c->over) {
            unsigned char* last = c->jlast + (size_t)s * MB_MAX;
            if (cand && carried) {
                unsigned sym = !x.any ? 0 : !memcmp(x.rmask, last, MB_MAX) ? 1 : 2;
                if (!(c->jnsym & 3)) rb_put(&c->jsym, "", 1);
                if (!c->jsym.oom) c->jsym.p[c->jsym.n - 1] |= (unsigned char)(sym << 2 * (c->jnsym & 3));
                c->jnsym++;
                if (sym == 1) j2_touch(c, last);
                else if (sym == 2) rb_u32(&c->ids, j2_id(c, x.rmask, last, 0));
            } else if (x.any) {
                if (cand) {
                    rb_varint(&c->jr, (uint32_t)((int64_t)nc - rprev - 1));
                    rprev = nc;
                    nr++;
                } else {
                    rb_varint(&c->jw, s - wprev - 1);
                    wprev = s;
                    nw++;
                }
                rb_u32(&c->ids, j2_id(c, x.rmask, last, 1));
            }
            if (cand && !carried) nc++;
            if (x.any) {
                unsigned mb = (c->p.nlanes + 7) / 8;
                memcpy(last, x.rmask, MB_MAX);
                c->jprev[s] = 1;
                rb_put(&c->lmask, x.rmask, mb);
                for (j = 0; j < c->p.nlanes; j++)
                    if (bit_of(x.rmask, j)) rb_u32(&c->lzz, x.zz[j]);
                if (c->lmask.n + c->lzz.n + c->ids.n + c->jrw.n > RECC_FRAME_CAP) c->over = 1;
            }
        }
        slot_advance(c, s, x.u, gt);
    }
    /* the iteration's ranks, then its woken slots, each a count and its gaps */
    rb_varint(&c->jrw, nr);
    rb_put(&c->jrw, c->jr.p, c->jr.n);
    rb_varint(&c->jrw, nw);
    rb_put(&c->jrw, c->jw.p, c->jw.n);
    pool_encode(c, pl, at, end);
    frames_walk(c, pl, at, end, it, gt);
    touched_end(c);
    return 1;
decline:
    for (k = 0; k < c->tl.n / 4; k++) c->touched[((const uint32_t*)c->tl.p)[k]] = 0;
    c->tl.n = 0;
    return 0;
}

/* RCS_UNITS from the block's listing and residuals (tagpu_recfmt.h): 0 when it passes
   RECC_FRAME_CAP or memory */
static int units_write(RecCodec* c, unsigned iterations)
{
    RecBytes* o = &c->sec[RCS_UNITS];
    unsigned mb = (c->p.nlanes + 7) / 8, j;
    size_t total = c->lmask.n / mb, nid = c->ids.n / 4, i, li, z;
    const uint32_t* ids = (const uint32_t*)c->ids.p;
    uint32_t* colv;
    unsigned char present[MB_MAX];
    size_t colat[RECC_LANES_MAX], coln[RECC_LANES_MAX];
    int wide = 0;
    o->n = 0;
    o->oom = 0;
    rb_varint(o, iterations);
    rb_varint(o, (uint32_t)c->jsym.n);
    rb_put(o, c->jsym.p, c->jsym.n);
    rb_put(o, c->jrw.p, c->jrw.n);
    rb_varint(o, (uint32_t)(c->jnew.n / mb));
    rb_put(o, c->jnew.p, c->jnew.n);
    for (i = 0; i < nid; i++) wide |= ids[i] > 0xFF;
    rb_put(o, wide ? "\1" : "", 1);
    for (i = 0; i < nid; i++) {
        if (wide) {
            rb_varint(o, ids[i]);
        } else {
            unsigned char b = (unsigned char)ids[i];
            rb_put(o, &b, 1);
        }
    }
    /* each lane's count: a bitmap of the lanes that have one, then those counts */
    memset(coln, 0, sizeof coln);
    for (i = 0; i < total; i++)
        for (j = 0; j < c->p.nlanes; j++) coln[j] += bit_of(c->lmask.p + i * mb, j);
    memset(present, 0, sizeof present);
    for (j = 0; j < c->p.nlanes; j++)
        if (coln[j]) present[j >> 3] |= (unsigned char)(1u << (j & 7));
    rb_put(o, present, mb);
    for (j = 0; j < c->p.nlanes; j++)
        if (coln[j]) rb_varint(o, (uint32_t)coln[j]);
    /* the columns: each lane's residuals in listed order */
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
    e->lmask.n = e->lzz.n = e->ids.n = e->jsym.n = e->jrw.n = e->jnew.n = 0;
    e->lmask.oom = e->lzz.oom = e->ids.oom = e->jsym.oom = e->jrw.oom = e->jnew.oom = 0;
    e->jr.oom = e->jw.oom = 0;
    e->jnsym = 0;
    e->jbad = 0;
    e->pctl.n = e->pref.n = e->psurv.n = e->pbirth.n = e->ptail.n = 0;
    e->pctl.oom = e->pref.oom = e->psurv.oom = e->pbirth.oom = e->ptail.oom = 0;
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
    if (e->over || e->kbad || e->jbad || e->lmask.oom || e->lzz.oom || e->ids.oom || e->jsym.oom ||
        e->jrw.oom || e->jnew.oom || e->jr.oom || e->jw.oom || e->pctl.oom || e->pref.oom ||
        e->psurv.oom || e->pbirth.oom || e->ptail.oom || !units_write(e, ticks) || !pool_write(e, ticks) ||
        !frames_write(e)) {
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

/* A walk over one iteration's records in its runs (kinds_parse): a run whose count is still open
   (group_fill) holds no record yet */
typedef struct RunAt { size_t run, end, cur; uint32_t left; } RunAt;

static void run_at(const RecCodec* d, RunAt* a, unsigned it)
{
    a->run = d->irun[it];
    a->end = d->irun[it + 1];
    a->cur = a->run;
    a->left = 0;
}

/* the next record's kind and its run's RUN_* flags, or 0 at the iteration's end */
static uint32_t run_next(const RecCodec* d, RunAt* a, uint32_t* flags)
{
    const uint32_t* runs = (const uint32_t*)d->runs.p;
    while (!a->left) {
        if (a->run >= a->end) return 0;
        a->cur = a->run++;
        a->left = runs[3 * a->cur + 1];
    }
    a->left--;
    *flags = runs[3 * a->cur + 2];
    return runs[3 * a->cur];
}

static void run_push(RecCodec* d, uint32_t kind, uint32_t count, uint32_t flags)
{
    rb_u32(&d->runs, kind);
    rb_u32(&d->runs, count);
    rb_u32(&d->runs, flags);
}

/* RCS_KINDS [at, end): every iteration's pattern into the decoder's runs, the pattern list moved
   on as the encoder moved it. BOUNDS: a reference within the list, a new pattern's length within
   1..RECC_KINDS_MAX and the section, each pattern's tokens within it and consuming it exactly,
   a kind within 1..0xFFFE or a marker, an explicit count of at least 1 and the records so far
   within the payload's dwords, the pattern's first token RK_TICK and no second RK_TICK run (its
   group's total is 1), the section consumed exactly. A group's first run is left open (count 0,
   RUN_DERIVED) for group_fill, but RK_TICK's, whose total is 1. */
static int kinds_parse(RecCodec* d, const unsigned char* sec, size_t at, size_t end,
                       unsigned iterations, uint32_t payload, char* why, unsigned cap)
{
    size_t records = 0;
    unsigned it;
    d->runs.n = 0;
    d->runs.oom = 0;
    for (it = 0; it < iterations; it++) {
        const unsigned char* p;
        size_t pat = 0, pn;
        unsigned m, seen = 0, first = 1;
        d->irun[it] = d->runs.n / 12;
        if (at >= end) REFUSE("iteration %u's pattern cut short", it);
        m = sec[at++];
        if (m > d->nkmru) REFUSE("iteration %u's pattern %u is past the %u the run holds", it, m, d->nkmru);
        if (m) {
            kmru_front(d, m - 1);
        } else {
            uint32_t len;
            if (!varint_get(sec, end, &at, &len) || len < 1 || len > RECC_KINDS_MAX || len > end - at)
                REFUSE("iteration %u's new pattern of a length outside 1..%u or past the kinds", it, RECC_KINDS_MAX);
            kmru_insert(d, sec + at, len);
            at += len;
            if (d->kbad) REFUSE("no memory for the kinds' patterns");
        }
        p = d->kmru[0].p;
        pn = d->kmru[0].n;
        while (pat < pn) {
            uint32_t kind, count = 0;
            int g;
            if (!varint_get(p, pn, &pat, &kind)) REFUSE("iteration %u's pattern cut short", it);
            if (first && kind != RK_TICK)
                REFUSE("iteration %u's pattern starts with kind %lu, not RK_TICK", it, (unsigned long)kind);
            if (kind == KM_TICKPAIR) {
                run_push(d, RK_EV_FEATURE, 1, RUN_IMPLICIT);
                run_push(d, RK_EV_FEATURE_END, 1, RUN_IMPLICIT);
                count = 2;
            } else if (kind == KM_FEATURES) {
                run_push(d, RK_FEATURES, 1, RUN_IMPLICIT);
                count = 1;
            } else if (kind > 0xFFFF) {
                REFUSE("record kind %lu in iteration %u's pattern", (unsigned long)kind, it);
            } else if ((g = group_of(kind)) >= 0 && !(seen >> g & 1)) {
                seen |= 1u << g;
                count = g == 0 ? 1 : 0;
                run_push(d, kind, count, RUN_DERIVED);
            } else {
                if (g == 0) REFUSE("iteration %u's pattern has a second RK_TICK run: the group's total is 1", it);
                if (!varint_get(p, pn, &pat, &count) || count < 1)
                    REFUSE("iteration %u's pattern has a run of kind %lu cut short or of no records", it, (unsigned long)kind);
                run_push(d, kind, count, 0);
            }
            if (count > payload / 4 - records) REFUSE("more records than a %lu-byte payload holds", (unsigned long)payload);
            records += count;
            first = 0;
        }
    }
    d->irun[iterations] = d->runs.n / 12;
    if (at != end) REFUSE("%lu bytes left in the kinds", (unsigned long)(end - at));
    if (d->runs.oom) REFUSE("no memory for the kinds");
    return 1;
}

/* Group g's open run of iteration `it` takes its count: the group's `total` from its frame less
   the group's other runs'. BOUND: a group with no run has a total of 0, and one whose other runs
   hold its total or more is refused -- its open run would hold fewer than 1 record. */
static int group_fill(RecCodec* d, unsigned it, int g, uint32_t total, const char* what, char* why,
                      unsigned cap)
{
    uint32_t* runs = (uint32_t*)d->runs.p;
    size_t i, open = (size_t)-1;
    uint64_t rest = 0;
    for (i = d->irun[it]; i < d->irun[it + 1]; i++) {
        if (group_of(runs[3 * i]) != g || runs[3 * i + 2] & RUN_IMPLICIT) continue;
        if (runs[3 * i + 2] & RUN_DERIVED) open = i;
        else rest += runs[3 * i + 1];
    }
    if (open == (size_t)-1) {
        if (total) REFUSE("iteration %u: the frame makes %lu %s, its pattern none", it, (unsigned long)total, what);
        return 1;
    }
    if (rest >= total)
        REFUSE("iteration %u: the pattern's runs hold %llu %s past the first, the frame makes %lu", it,
               (unsigned long long)rest, what, (unsigned long)total);
    runs[3 * open + 1] = (uint32_t)(total - rest);
    return 1;
}

/* the 27 residuals of one row from its columns' cursors (read once already) */
static void cols_get(const unsigned char* sec, size_t* colat, const size_t* colend, unsigned first,
                     int order, uint32_t* res)
{
    unsigned i;
    uint32_t z = 0;
    for (i = 0; i < PDW; i++) {
        varint_get(sec, colend[first + i], &colat[first + i], &z);
        res[order ? PORDER[i] : i] = unzz32(z);
    }
}

/* The pool frame's cursors across a block's iterations: pool_parse sets them, each pool_iter
   moves them on. */
typedef struct PoolAt {
    size_t end;                         /* RCS_PROJ's end                                      */
    size_t refat;                       /* the next reference code                             */
    size_t colat[3 * PDW], colend[3 * PDW];
    size_t total;                       /* the projectile records' bytes so far, padded        */
} PoolAt;

/* The block's pool frame, RCS_PROJ [at, end), read and bounded before its first iteration: the
   control values against the pool (nproj) and the head's iterations, then the codes and the
   columns against the section's bytes. 1, or 0 with `why`. */
static int pool_parse(RecCodec* d, const unsigned char* sec, size_t at, size_t end,
                      unsigned iterations, PoolAt* q, char* why, unsigned cap)
{
    const unsigned N = d->nproj;
    uint64_t c0 = d->pcount, h0 = d->phw, nsall = 0, nball = 0, ntall = 0, need, k;
    uint32_t v;
    unsigned it, i;
    if (!varint_get(sec, end, &at, &v) || v != iterations)
        REFUSE("the pool frame's iterations are not the head's %u", iterations);
    for (it = 0; it < iterations; it++) {
        uint64_t c1, h1, nd, nt;
        int64_t last;
        d->pat[it] = at;
        if (!varint_get(sec, end, &at, &v)) REFUSE("iteration %u's count cut short", it);
        last = (int64_t)c0 + unzz64(v);
        if (last < 0 || last > N) REFUSE("iteration %u's count %lld is outside 0..%u", it, (long long)last, N);
        c1 = (uint64_t)last;
        if (!varint_get(sec, end, &at, &v)) REFUSE("iteration %u's extent cut short", it);
        h1 = h0 + v;
        if (h1 > N) REFUSE("iteration %u's extent %llu is past the pool's %u", it, (unsigned long long)h1, N);
        if (c1 > h1) REFUSE("iteration %u's count %llu is past its extent %llu", it, (unsigned long long)c1, (unsigned long long)h1);
        if (!varint_get(sec, end, &at, &v)) REFUSE("iteration %u's deaths cut short", it);
        nd = v;
        if (nd > c0) REFUSE("iteration %u: %llu deaths among %llu records", it, (unsigned long long)nd, (unsigned long long)c0);
        for (last = -1, k = 0; k < nd; k++) {
            if (!varint_get(sec, end, &at, &v)) REFUSE("iteration %u's dead slots cut short", it);
            last += 1 + (int64_t)v;
            if (last >= (int64_t)c0) REFUSE("iteration %u: a dead slot %lld past the %llu records", it, (long long)last, (unsigned long long)c0);
        }
        if (c0 - nd > c1) REFUSE("iteration %u: %llu survivors past its count %llu", it, (unsigned long long)(c0 - nd), (unsigned long long)c1);
        if (!varint_get(sec, end, &at, &v)) REFUSE("iteration %u's tail cut short", it);
        nt = v;
        if (nt > h1 - c1) REFUSE("iteration %u: %llu tail slots past the count", it, (unsigned long long)nt);
        for (last = (int64_t)c1 - 1, k = 0; k < nt; k++) {
            if (!varint_get(sec, end, &at, &v)) REFUSE("iteration %u's tail slots cut short", it);
            last += 1 + (int64_t)v;
            if (last >= (int64_t)h1) REFUSE("iteration %u: a tail slot %lld past the extent", it, (long long)last);
        }
        nsall += c0 - nd;
        nball += c1 - (c0 - nd);
        ntall += nt;
        c0 = c1;
        h0 = h1;
    }
    /* one byte at least a varint: checked before the walk the counts size */
    need = nball + ntall + (uint64_t)PDW * (nsall + nball + ntall);
    if (need > end - at)
        REFUSE("the pool frame's codes and columns need %llu bytes, %lu are left", (unsigned long long)need, (unsigned long)(end - at));
    q->refat = at;
    for (k = 0; k < nball + ntall; k++)
        if (!varint_get(sec, end, &at, &v)) REFUSE("a reference code cut short at byte %lu", (unsigned long)at);
    for (i = 0; i < 3 * PDW; i++) {
        uint64_t n = i < PDW ? nsall : i < 2 * PDW ? nball : ntall;
        q->colat[i] = at;
        for (k = 0; k < n; k++)
            if (!varint_get(sec, end, &at, &v)) REFUSE("pool frame column %u cut short at byte %lu", i, (unsigned long)at);
        q->colend[i] = at;
    }
    if (at != end) REFUSE("%lu bytes left in the pool frame", (unsigned long)(end - at));
    q->end = end;
    q->total = 0;
    return 1;
}

/* Iteration `it` of the pool frame: its control values (bounded by
   pool_parse), the EV rows from the last iteration's shots -- d->shots, which that iteration's
   pass B took from the bodies the events frame made -- the new pool, and each projectile record's
   body made from the pool before and after it into d->pb, in record order, the iteration's
   projectile runs taking their count from the changed records (group_fill); then the state
   committed. Runs before the iteration's pass A and pass B, which takes the bodies from d->pb. 1,
   or 0 with `why`. */
static int pool_iter(RecCodec* d, const unsigned char* sec, PoolAt* q, unsigned it, size_t payload,
                     char* why, unsigned cap)
{
    const unsigned N = d->nproj, pc0 = d->pcount, ph0 = d->phw, ne = d->nshots;
    unsigned pc1, ph1, nd, nt, ns = 0, heads = 0, k, i, j;
    size_t cat = d->pat[it], end = q->end;
    uint32_t kind, fl, res[PDW], code = 0, v = 0;
    RunAt a;
    d->pb.n = 0;
    d->pb.oom = 0;
    /* the control values, read and bounded by pool_parse */
    varint_get(sec, end, &cat, &v);
    pc1 = (unsigned)((int64_t)pc0 + unzz64(v));
    varint_get(sec, end, &cat, &v);
    ph1 = ph0 + v;
    varint_get(sec, end, &cat, &v);
    nd = v;
    for (i = 0; i < pc0; i++) d->pinv[i] = 0;
    for (i = 0, j = 0; i < nd; i++) {
        varint_get(sec, end, &cat, &v);
        j += v;
        d->pinv[j++] = -1;
    }
    for (i = 0; i < pc0; i++)
        if (!d->pinv[i]) {
            d->pinv[i] = (int32_t)ns;
            d->pm[ns++] = (int32_t)i;
        } else {
            d->pinv[i] = -1;
        }
    varint_get(sec, end, &cat, &v);
    nt = v;
    for (i = 0, j = pc1; i < nt; i++) {
        varint_get(sec, end, &cat, &v);
        j += v;
        d->ptl[i] = j++;
    }
    /* the EV rows, from the shots and the last births as they stand before this iteration */
    for (i = 0; i < ne; i++) ev_row(d, d->shots + (size_t)i * SHOT, d->ev + (size_t)i * PDW);
    /* the iteration's RK_PROJ_HEAD, which the pattern counts */
    run_at(d, &a, it);
    while ((kind = run_next(d, &a, &fl)) != 0) heads += kind == RK_PROJ_HEAD;
    if (heads > 1) REFUSE("iteration %u has %u RK_PROJ_HEAD", it, heads);
    if (!heads && (pc1 != pc0 || ph1 != ph0)) REFUSE("iteration %u changes the pool's count or extent with no RK_PROJ_HEAD", it);
    /* the new pool: the survivors, the births, the tail, the rest as they were */
    memcpy(d->pool_n, d->pool, (size_t)ph0 * PIMG);
    for (j = 0; j < ns; j++) {
        unsigned m = (unsigned)d->pm[j];
        memcpy(d->pwin_n + (size_t)j * PDW, d->pwin + (size_t)m * PDW, PDW);
        cols_get(sec, q->colat, q->colend, 0, 1, res);
        pool_step(d, m, d->pool_n + (size_t)j * PDW, res, d->pwin_n + (size_t)j * PDW, 1);
        memcpy(d->pool_l2n + (size_t)j * PDW, d->pool + (size_t)m * PDW, PIMG);
        d->page_n[j] = (unsigned char)(d->page[m] >= 2 ? 2 : d->page[m] + 1);
    }
    memset(d->adv_done, 0, pc0);
    d->evnext = 0;
    for (j = ns; j < pc1; j++) {
        uint32_t* row = d->pool_n + (size_t)j * PDW;
        const uint32_t* R = NULL;
        varint_get(sec, end, &q->refat, &code);
        if (code) {
            unsigned sc = (code - 1) & 3, len[4];
            int64_t anchor[4], kk;
            len[PS_OUT] = j;
            len[PS_OLD] = ph0;
            len[PS_ADV] = pc0;
            len[PS_EV] = ne;
            anchor[PS_OUT] = (int64_t)j - 1;
            anchor[PS_OLD] = anchor[PS_ADV] = (int64_t)pc0 - 1;
            anchor[PS_EV] = d->evnext < ne ? (int64_t)d->evnext : (int64_t)ne - 1;
            kk = anchor[sc] + unzz64((code - 1) >> 2);
            if (kk < 0 || kk >= (int64_t)len[sc])
                REFUSE("iteration %u: birth %u's reference %lld is past section %u's %u rows", it, j - ns, (long long)kk, sc, len[sc]);
            R = sec_row(d, sc, (unsigned)kk);
            if (sc == PS_EV) d->evnext = (unsigned)kk + 1;
        }
        cols_get(sec, q->colat, q->colend, PDW, 0, res);
        ref_row(R, 1, (pc0 + j - ns) & 0xFFFF, res, row, 1);
        if (row[0] >= 1 && row[0] <= RECC_WEAPON_IDS) memcpy(d->lastnew + (size_t)row[0] * PDW, row, PIMG);
        memset(d->pwin_n + (size_t)j * PDW, 0, PDW);
        d->page_n[j] = 1;
    }
    for (i = 0; i < nt; i++) {
        unsigned tj = d->ptl[i];
        const uint32_t* R = NULL;
        varint_get(sec, end, &q->refat, &code);
        if (code) {
            unsigned sc = (code - 1) & 3, len[4];
            int64_t anchor[4], kk;
            len[PS_OUT] = pc1;
            len[PS_OLD] = ph0;
            len[PS_ADV] = pc0;
            len[PS_EV] = ne;
            anchor[PS_OUT] = tj < pc0 ? (d->pinv[tj] >= 0 ? d->pinv[tj] : (tj < pc1 ? tj : (int64_t)pc1 - 1))
                                      : (ns + (tj - pc0) < pc1 ? ns + (tj - pc0) : (int64_t)pc1 - 1);
            anchor[PS_OLD] = tj < ph0 ? tj : (int64_t)ph0 - 1;
            anchor[PS_ADV] = tj < pc0 ? tj : (int64_t)pc0 - 1;
            anchor[PS_EV] = (int64_t)ne - 1;
            kk = anchor[sc] + unzz64((code - 1) >> 2);
            if (kk < 0 || kk >= (int64_t)len[sc])
                REFUSE("iteration %u: tail slot %u's reference %lld is past section %u's %u rows", it, tj, (long long)kk, sc, len[sc]);
            R = sec_row(d, sc, (unsigned)kk);
        }
        cols_get(sec, q->colat, q->colend, 2 * PDW, 0, res);
        ref_row(R, 0, 0, res, d->pool_n + (size_t)tj * PDW, 1);
    }
    /* the changed slots: below the count, then the tail (the pool is zero from ph0 on) */
    d->nchg = 0;
    for (j = 0; j < pc1; j++)
        if (memcmp(d->pool_n + (size_t)j * PDW, d->pool + (size_t)j * PDW, PIMG)) d->pchg[d->nchg++] = j;
    for (i = 0; i < nt; i++)
        if (memcmp(d->pool_n + (size_t)d->ptl[i] * PDW, d->pool + (size_t)d->ptl[i] * PDW, PIMG))
            d->pchg[d->nchg++] = d->ptl[i];
    if (!group_fill(d, it, 2, d->nchg, "projectile records", why, cap)) return 0;
    /* their bodies, in record order */
    run_at(d, &a, it);
    k = 0;
    while ((kind = run_next(d, &a, &fl)) != 0) {
        unsigned char body[POOL_BODY_MAX];
        unsigned len;
        if (kind == RK_PROJ_HEAD) {
            put32(body, pc1);
            put32(body + 4, ph1);
            len = 8;
        } else if (kind == RK_PROJ || kind == RK_PROJ_DELTA) {
            j = d->pchg[k++];
            len = pool_body(kind, j, d->pool + (size_t)j * PDW, d->pool_n + (size_t)j * PDW, body);
        } else {
            continue;
        }
        q->total += (4 + (size_t)len + 3) & ~(size_t)3;
        if (q->total > payload) REFUSE("the records pass the head's %lu-byte payload", (unsigned long)payload);
        rb_u32(&d->pb, len);
        rb_put(&d->pb, body, len);
    }
    if (d->pb.oom) REFUSE("no memory for the projectile records");
    /* the commit */
    memset(d->page_n + pc1, 0, N - pc1);
    {
        uint32_t* t = d->pool; d->pool = d->pool_n; d->pool_n = t;
        t = d->pool_l2; d->pool_l2 = d->pool_l2n; d->pool_l2n = t;
    }
    {
        unsigned char* t = d->pwin; d->pwin = d->pwin_n; d->pwin_n = t;
        t = d->page; d->page = d->page_n; d->page_n = t;
    }
    d->pcount = pc1;
    d->phw = ph1;
    return 1;
}

int reccodec_decode(RecCodec* d, const unsigned char* sec, size_t n, RecBytes* out, char* why,
                    unsigned cap)
{
    RecSecHead h;
    size_t off[RCS_COUNT + 1], at, i, oat, uend, symat, nsymb, nid, inext = 0, jsi = 0;
    size_t colat[RECC_LANES_MAX], colend[RECC_LANES_MAX], rwat[RECC_TICKS_MAX];
    unsigned it, mb, j, ndict = 0, dnext = 0;
    uint32_t v, kind, fl, gt0 = 0;
    const unsigned char* dict;
    const unsigned char* present;
    PoolAt pq;
    if (!d->valid) REFUSE("a sectioned block with no frame to continue");
    d->valid = 0;
    d->tl.n = 0;
    if (n < sizeof h) REFUSE("a sectioned block of %lu bytes, under its %u-byte head", (unsigned long)n, (unsigned)sizeof h);
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

    /* RCS_KINDS: one pattern an iteration, each starting with its RK_TICK */
    if (!kinds_parse(d, sec, off[RCS_KINDS], off[RCS_KINDS + 1], h.iterations, h.payload, why, cap))
        return 0;

    /* RCS_UNITS: everything read and bounded before the first iteration but what the listing
       needs the decode's state for, which each iteration bounds (THE LISTING) */
    at = off[RCS_UNITS];
    uend = off[RCS_UNITS + 1];
    mb = (d->p.nlanes + 7) / 8;
    if (!varint_get(sec, uend, &at, &v) || v != h.iterations)
        REFUSE("the unit frame's iterations are not the head's %u", h.iterations);
    if (!varint_get(sec, uend, &at, &v) || v > uend - at) REFUSE("the unit frame's symbols run past it");
    symat = at;
    nsymb = v;
    at += nsymb;
    nid = 0;
    for (i = 0; i < 4 * nsymb; i++) {
        unsigned sym = sec[symat + (i >> 2)] >> 2 * (i & 3) & 3;
        if (sym == 3) REFUSE("symbol %lu is 3", (unsigned long)i);
        nid += sym == 2;
    }
    for (it = 0; it < h.iterations; it++) {
        unsigned k, pass;
        rwat[it] = at;
        for (pass = 0; pass < 2; pass++) {               /* the ranks, then the woken slots */
            uint64_t prev = pass ? 0 : (uint64_t)-1;
            if (!varint_get(sec, uend, &at, &v) || v > d->nslots)
                REFUSE("iteration %u lists past the %u slots, at byte %lu", it, d->nslots, (unsigned long)at);
            nid += v;
            for (k = 0, i = v; k < i; k++) {
                if (!varint_get(sec, uend, &at, &v)) REFUSE("a gap cut short at byte %lu", (unsigned long)at);
                prev += (uint64_t)v + 1;
                if (prev >= d->nslots) REFUSE("iteration %u names %llu, past the %u slots", it, (unsigned long long)prev, d->nslots);
            }
        }
    }
    if (!varint_get(sec, uend, &at, &v)) REFUSE("the mask count cut short at byte %lu", (unsigned long)at);
    if (v > nid) REFUSE("%lu new masks for %lu ids", (unsigned long)v, (unsigned long)nid);
    ndict = v;
    if ((size_t)ndict * mb > uend - at) REFUSE("the masks run past the unit frame");
    dict = sec + at;
    for (i = 0; i < ndict; i++) {
        const unsigned char* m = dict + i * mb;
        unsigned any = 0;
        for (j = 0; j < mb; j++) any |= m[j];
        if (!any) REFUSE("mask %lu is empty", (unsigned long)i);
        if (d->p.nlanes % 8 && m[mb - 1] >> (d->p.nlanes % 8)) REFUSE("mask %lu names a lane past %u", (unsigned long)i, d->p.nlanes);
    }
    at += (size_t)ndict * mb;
    if (at >= uend || sec[at] > 1) REFUSE("the ids' mode is not 0 or 1");
    d->ids.n = 0;
    d->ids.oom = 0;
    for (i = 0, j = sec[at++]; i < nid; i++) {
        if (!j) {
            if (at >= uend) REFUSE("the mask ids cut short");
            v = sec[at++];
        } else if (!varint_get(sec, uend, &at, &v)) {
            REFUSE("a mask id cut short at byte %lu", (unsigned long)at);
        }
        rb_u32(&d->ids, v);
    }
    if (d->ids.oom) REFUSE("no memory for the mask ids");
    /* each lane's count, then its column: every residual read and bounded here, each lane's
       consumed by the listed masks to exactly its end (after the last iteration) */
    if (mb > uend - at) REFUSE("the lanes' counts cut short");
    present = sec + at;
    at += mb;
    if (d->p.nlanes % 8 && present[mb - 1] >> (d->p.nlanes % 8)) REFUSE("a count for a lane past %u", d->p.nlanes);
    {
        uint32_t cnt[RECC_LANES_MAX];
        for (j = 0; j < d->p.nlanes; j++) {
            cnt[j] = 0;
            if (bit_of(present, j) && (!varint_get(sec, uend, &at, &cnt[j]) || !cnt[j]))
                REFUSE("lane %u's count cut short or 0", j);
        }
        for (j = 0; j < d->p.nlanes; j++) {
            unsigned w = d->p.lane[j].width;
            colat[j] = at;
            for (i = 0; i < cnt[j]; i++) {
                if (!varint_get(sec, uend, &at, &v)) REFUSE("lane %u's column cut short at byte %lu", j, (unsigned long)at);
                if (w < 4 && v >> (8 * w)) REFUSE("lane %u's residual %lu is past its %u bytes", j, (unsigned long)v, w);
            }
            colend[j] = at;
        }
    }
    if (at != uend) REFUSE("%lu bytes left in the unit frame", (unsigned long)(uend - at));

    /* RCS_PROJ and the record frames' sections, read and bounded before the first iteration */
    if (!pool_parse(d, sec, off[RCS_PROJ], off[RCS_PROJ + 1], h.iterations, &pq, why, cap)) return 0;
    for (j = 0; j < RECC_FRAMES; j++) {
        unsigned s = FRAMES[j]->section;
        if (!FRAMES[j]->dec_open(d->frame[j], sec + off[s], off[s + 1] - off[s], h.iterations, why, cap))
            return 0;
    }

    out->n = 0;
    out->oom = 0;
    if (!rb_grow(out, h.payload)) REFUSE("no memory for a %lu-byte payload", (unsigned long)h.payload);

    /* the iterations: the pool frame's, pass A, the unit frame, pass B. The pool frame fills its
       group's count before pass A; pass A walks every run but RK_UNIT_DELTA's, whose count the
       unit frame fills after it (THE KINDS). */
    oat = off[RCS_OTHER];
    for (it = 0; it < h.iterations; it++) {
        RunAt a;
        size_t aoat = oat, dat = 0, pbat = 0, rat = rwat[it], nr, nw, ri = 0, wi = 0, nc = 0;
        unsigned nchanged = 0, s, nshot = 0;
        unsigned char mk[MB_MAX];
        uint32_t gt = gt0 + it;
        unsigned char tick[4], root[8];
        int rooted = 0;
        RecFrameAt fa;
        Eval x;
        if (!pool_iter(d, sec, &pq, it, h.payload, why, cap)) return 0;
        /* pass A: the iteration's RK_TICK and unit records, from copies of the cursors */
        d->dl.n = 0;
        d->dl.oom = 0;
        run_at(d, &a, it);
        while ((kind = run_next(d, &a, &fl)) != 0) {
            uint32_t len;
            const unsigned char* b;
            /* a frame or a marker makes it in pass B; the block's first RK_TICK alone has a body */
            if (kind == RK_UNIT_DELTA || fl & RUN_IMPLICIT || rec_section_of(kind) != RCS_OTHER ||
                (kind == RK_TICK && it))
                continue;
            if (!varint_get(sec, off[RCS_OTHER + 1], &aoat, &len) || len > 0xFFFF || len > off[RCS_OTHER + 1] - aoat)
                REFUSE("a body of kind %lu cut short or past 0xFFFF in section %u at byte %lu",
                       (unsigned long)kind, RCS_OTHER, (unsigned long)aoat);
            b = sec + aoat;
            aoat += len;
            if (kind == RK_TICK) {
                if (len != 4) REFUSE("an RK_TICK of %lu bytes", (unsigned long)len);
                gt = gt0 = get32(b);
            } else if (kind == RK_UNIT || kind == RK_UNIT_GONE) {
                if (len != (kind == RK_UNIT ? 4u + IMG : 4u)) REFUSE("a unit record of kind %lu of %lu bytes", (unsigned long)kind, (unsigned long)len);
                if (get16(b) < 1 || get16(b) >= d->nslots) REFUSE("a unit record for slot %u, outside 1..%u", get16(b), d->nslots - 1);
                touch(d, get16(b));
            }
        }
        if (d->tl.oom) REFUSE("no memory for the touched slots");
        /* the iteration's ranks and woken slots, read and bounded above */
        d->jr.n = d->jw.n = 0;
        {
            uint64_t prev = (uint64_t)-1;
            varint_get(sec, uend, &rat, &v);
            for (nr = v, i = 0; i < nr; i++) {
                varint_get(sec, uend, &rat, &v);
                prev += (uint64_t)v + 1;
                rb_u32(&d->jr, (uint32_t)prev);
            }
            varint_get(sec, uend, &rat, &v);
            for (nw = v, i = 0, prev = 0; i < nw; i++) {
                varint_get(sec, uend, &rat, &v);
                prev += (uint64_t)v + 1;
                rb_u32(&d->jw, (uint32_t)prev);
            }
        }
        if (d->jr.oom || d->jw.oom) REFUSE("no memory for the listing");
        /* the frame: the candidates in ascending order and the woken slots, the listed ones
           among them (THE LISTING) */
        memset(&x, 0, sizeof x);
        x.c = d;
        x.gt = gt;
        x.dec = 1;
        x.sec = sec;
        x.colend = colend;
        x.colat = colat;
        for (s = 1; s < d->nslots; s++) {
            int carried = d->jprev[s], lst = 0, may0 = 1;
            unsigned sym = 0;
            unsigned char* last = d->jlast + (size_t)s * MB_MAX;
            unsigned char* l1;
            d->jprev[s] = 0;
            if (!d->live[s] || d->touched[s]) continue;
            if (d->rest[s]) {
                lst = wi < nw && ((const uint32_t*)d->jw.p)[wi] == s;
                wi += lst;
                if (!lst) continue;
            } else if (carried) {
                if (jsi >= 4 * nsymb) REFUSE("iteration %u: the symbols end before carried slot %u", it, s);
                sym = sec[symat + (jsi >> 2)] >> 2 * (jsi & 3) & 3;
                jsi++;
                lst = sym != 0;
                may0 = 0;
            } else {
                lst = ri < nr && ((const uint32_t*)d->jr.p)[ri] == nc;
                ri += lst;
                nc++;
            }
            if (lst) {
                if (sym == 1) {
                    memcpy(mk, last, MB_MAX);
                    if (!mask_any(mk)) REFUSE("iteration %u: slot %u takes its last mask, which is empty", it, s);
                    j2_touch(d, mk);
                } else {
                    if (inext >= nid) REFUSE("iteration %u: the mask ids end before slot %u", it, s);
                    v = ((const uint32_t*)d->ids.p)[inext++];
                    if (!j2_mask(d, v, last, may0, dict, ndict, &dnext, mk))
                        REFUSE("iteration %u: mask id %lu for slot %u names no mask", it, (unsigned long)v, s);
                }
                memcpy(last, mk, MB_MAX);
                d->jprev[s] = 1;
            }
            l1 = (unsigned char*)(d->unit + (size_t)s * NDW);
            x.l1 = l1;
            x.l2 = (const unsigned char*)(d->l2 + (size_t)s * NDW);
            x.l3 = d->l3 + (size_t)s * L3B;
            x.win = d->win + (size_t)s * RECC_LANES_MAX;
            x.age = d->age[s];
            x.s = s;
            x.mask = lst ? mk : NULL;
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
        if (ri != nr || wi != nw)
            REFUSE("iteration %u: a rank or a woken slot names no slot it can list", it);
        if (d->dl.oom) REFUSE("no memory for the deltas");
        if (!group_fill(d, it, 1, nchanged, "unit deltas", why, cap)) return 0;
        /* pass B: the records, in order; a frame record's body from its frame, at the unit copy
           and the events flag the encoder's walk had there (frames_walk) */
        fa.it = it;
        fa.gt = gt;
        fa.events = 0;
        fa.nslots = d->nslots;
        fa.unit = d->unit;
        fa.live = d->live;
        for (j = 0; j < RECC_FRAMES; j++)
            if (!FRAMES[j]->dec_iter(d->frame[j], &fa, why, cap)) return 0;
        run_at(d, &a, it);
        while ((kind = run_next(d, &a, &fl)) != 0) {
            unsigned sc = rec_section_of(kind);
            uint32_t len;
            unsigned char hd[4];
            const unsigned char* b;
            size_t rl;
            if (fl & RUN_IMPLICIT) {
                /* a marker's records: no frame codes them. BOUND: each only after its explicit
                   kind since the reset, as the encoder writes it (frames_walk). */
                if (kind == RK_FEATURES) {
                    void* st = d->frame[d->fix[RCS_STATE]];
                    if (!d->featseen) REFUSE("iteration %u: a predicted RK_FEATURES before any explicit one", it);
                    recframe_state_predicted(st, d->fbody);
                    recframe_state_take(st, d->fbody);
                    b = d->fbody;
                    len = 16;
                } else {
                    if (!d->tpaired) REFUSE("iteration %u: an empty feature tick before any explicit one", it);
                    b = d->tpair + (kind == RK_EV_FEATURE_END ? 8 : 0);
                    len = 8;
                }
            } else if (kind == RK_TICK && it) {
                put32(tick, gt);
                b = tick;
                len = 4;
            } else if (kind == RK_UNIT_DELTA) {
                memcpy(&len, d->dl.p + dat, 4);
                b = d->dl.p + dat + 4;
                dat += 4 + len;
            } else if (sc == RCS_PROJ) {
                memcpy(&len, d->pb.p + pbat, 4);
                b = d->pb.p + pbat + 4;
                pbat += 4 + len;
            } else if (sc == RCS_OTHER) {
                varint_get(sec, off[RCS_OTHER + 1], &oat, &len);    /* read in pass A */
                b = sec + oat;
                oat += len;
                if (kind == RK_UNIT || kind == RK_UNIT_GONE) unit_record(d, kind, b);
            } else {
                const RecFrameOps* f = FRAMES[d->fix[sc]];
                unsigned bl = 0;
                if (!f->dec_rec(d->frame[d->fix[sc]], &fa, kind, d->fbody, &bl, why, cap)) return 0;
                if (bl > RECF_BODY_MAX) REFUSE("the %s frame made a body of %u bytes", f->name, bl);
                len = bl;
                b = d->fbody;
                if (kind == RK_EV_SHOT && len == SHOT && nshot < RECC_SHOTS_MAX)
                    memcpy(d->shots + (size_t)nshot++ * SHOT, b, SHOT);
            }
            /* the run's last explicit empty feature tick (tick_pair), and the first explicit
               RK_FEATURES, in the record order the encoder's walk kept them */
            if (!(fl & RUN_IMPLICIT)) {
                if (rooted && kind == RK_EV_FEATURE_END && len == 8 && b[0] == FR_TICK) {
                    memcpy(d->tpair, root, 8);
                    memcpy(d->tpair + 8, b, 8);
                    d->tpaired = 1;
                }
                rooted = kind == RK_EV_FEATURE && len == 8 && b[0] == FR_TICK;
                if (rooted) memcpy(root, b, 8);
                if (kind == RK_FEATURES) d->featseen = 1;
            } else {
                rooted = 0;
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
            if (sc == RCS_EVENTS) fa.events = 1;
        }
        for (j = 0; j < RECC_FRAMES; j++)
            if (!FRAMES[j]->dec_iter_end(d->frame[j], &fa, why, cap)) return 0;
        /* the next iteration's pool takes these shots (pool_iter) */
        d->nshots = nshot;
        touched_end(d);
    }
    if (oat != off[RCS_OTHER + 1]) REFUSE("%lu bytes left in section %u", (unsigned long)(off[RCS_OTHER + 1] - oat), RCS_OTHER);
    if (nsymb != (jsi + 3) / 4 || (jsi & 3 && sec[symat + nsymb - 1] >> 2 * (jsi & 3)))
        REFUSE("%lu symbol bytes for %lu carried slots", (unsigned long)nsymb, (unsigned long)jsi);
    if (inext != nid || dnext != ndict)
        REFUSE("%lu of %lu mask ids and %u of %u new masks taken", (unsigned long)inext, (unsigned long)nid, dnext, ndict);
    for (j = 0; j < d->p.nlanes; j++)
        if (colat[j] != colend[j]) REFUSE("lane %u's column is not consumed by the listed masks", j);
    for (j = 0; j < RECC_FRAMES; j++)
        if (!FRAMES[j]->dec_close(d->frame[j], why, cap)) return 0;
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
   (ZSTD_defaultCParameters), or the block's own size when that is smaller, and a snapshot's is the
   least power of two that holds it and its prefix (reccodec_pack_chained). */
#define RECC_WINDOW_MAX (1ull << 27)
#define RECC_LEVEL_MAX  19            /* the run's and the snapshots' level                     */
/* Their match finder's tables, fixed so that the context does not grow with a snapshot's size:
   MEASURED 2026-10-11 on the heavy hour, the level-19 context is 14.5 MB at these and the run's
   window, and 33.3 MB at level 19's own parameters for the snapshots. */
#define RECC_HASH_LOG   20
#define RECC_CHAIN_LOG  21

/* one zstd frame of exactly `raw` bytes and no byte after it, whose window is bounded */
static int frame_ok(const unsigned char* src, size_t packed, size_t raw)
{
    ZSTD_FrameHeader fh;
    if (!raw || !packed) return 0;
    if (ZSTD_findFrameCompressedSize(src, packed) != packed) return 0;
    if (ZSTD_getFrameContentSize(src, packed) != (unsigned long long)raw) return 0;
    return ZSTD_getFrameHeader(&fh, src, packed) == 0 && fh.frameType == ZSTD_frame &&
           fh.windowSize <= RECC_WINDOW_MAX;
}

/* The decoder is reset first, parameters and prefix included: a prefix a refused frame left
   referenced would otherwise be used by the next frame unpacked here. */
int reccodec_unpack(void* dctx, unsigned char* dst, size_t raw, const unsigned char* src,
                    size_t packed)
{
    if (!frame_ok(src, packed, raw)) return 0;
    ZSTD_DCtx_reset((ZSTD_DCtx*)dctx, ZSTD_reset_session_and_parameters);
    return ZSTD_decompressDCtx((ZSTD_DCtx*)dctx, dst, raw, src, packed) == raw;
}

int reccodec_unpack_chained(void* dctx, unsigned char* dst, size_t raw, const unsigned char* src,
                            size_t packed, const unsigned char* prefix, size_t nprefix)
{
    ZSTD_DCtx* d = (ZSTD_DCtx*)dctx;
    if (!prefix || !nprefix || nprefix > RECC_PAYLOAD_MAX || !frame_ok(src, packed, raw)) return 0;
    ZSTD_DCtx_reset(d, ZSTD_reset_session_and_parameters);
    if (ZSTD_isError(ZSTD_DCtx_refPrefix(d, prefix, nprefix))) return 0;
    return ZSTD_decompressDCtx(d, dst, raw, src, packed) == raw;
}

/* A window of at least 2^20 holding the prefix and the records together, so every byte of the
   prefix stays in reach of the last record (a match past the window is never made). */
int reccodec_pack_chained(void* cctx, RecBytes* out, const unsigned char* src, size_t n,
                          const unsigned char* prefix, size_t nprefix)
{
    ZSTD_CCtx* c = (ZSTD_CCtx*)cctx;
    unsigned long long both = (unsigned long long)nprefix + n;
    int wlog = REC_RUN_WINDOW_LOG;
    size_t z;
    out->n = 0;
    out->oom = 0;
    if (!n || both > RECC_WINDOW_MAX || !rb_grow(out, ZSTD_compressBound(n))) return 0;
    while ((1ull << wlog) < both) wlog++;
    if (ZSTD_isError(ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, RECC_LEVEL_MAX)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, wlog)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_hashLog, RECC_HASH_LOG)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_chainLog, RECC_CHAIN_LOG)) ||
        (nprefix && ZSTD_isError(ZSTD_CCtx_refPrefix(c, prefix, nprefix))))
        return 0;
    z = ZSTD_compress2(c, out->p, out->cap, src, n);
    if (ZSTD_isError(z)) return 0;
    out->n = z;
    return 1;
}

/* BOUNDS: a run's header is the writer's or refused -- a zstd frame, its window at most
   2^REC_RUN_WINDOW_LOG, no content size, no dictionary, no checksum -- and the decoder's own
   window limit is set to the same, so no chunk can make it allocate past it. The output buffer
   is raw + 1 bytes: a chunk that gives more than raw stops there and is refused, as is one that
   ends the frame (ZSTD_decompressStream's 0), which the writer never does. Every call either
   consumes input or makes output, else the chunk is refused, so the loop ends. */
int reccodec_run_unpack(void* dctx, unsigned char* dst, size_t raw, const unsigned char* src,
                        size_t packed, int reset)
{
    ZSTD_DCtx* d = (ZSTD_DCtx*)dctx;
    ZSTD_inBuffer in;
    ZSTD_outBuffer out;
    if (!raw || !packed) return 0;
    if (reset) {
        ZSTD_FrameHeader fh;
        if (packed < 4 || get32(src) != ZSTD_MAGICNUMBER ||
            ZSTD_getFrameHeader(&fh, src, packed) != 0 || fh.frameType != ZSTD_frame ||
            fh.windowSize > (1ull << REC_RUN_WINDOW_LOG) ||
            fh.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN || fh.dictID || fh.checksumFlag)
            return 0;
        if (ZSTD_isError(ZSTD_DCtx_reset(d, ZSTD_reset_session_and_parameters)) ||
            ZSTD_isError(ZSTD_DCtx_setParameter(d, ZSTD_d_windowLogMax, REC_RUN_WINDOW_LOG)))
            return 0;
    }
    in.src = src;
    in.size = packed;
    in.pos = 0;
    out.dst = dst;
    out.size = raw + 1;
    out.pos = 0;
    for (;;) {
        size_t was_in = in.pos, was_out = out.pos, k = ZSTD_decompressStream(d, &out, &in);
        if (ZSTD_isError(k) || !k || out.pos > raw) return 0;
        if (in.pos == was_in && out.pos == was_out) break;
    }
    return in.pos == in.size && out.pos == raw;
}

/* ---- the writer's codec --------------------------------------------------------------------- */

int reccodec_stream_begin(RecStream* s, const RecCodecParams* p)
{
    memset(s, 0, sizeof *s);
    s->p = *p;
    s->enc = reccodec_new(p);
    s->chk = reccodec_new(p);
    s->cctx = ZSTD_createCCtx();
    s->dctx = ZSTD_createDCtx();
    s->zrun = ZSTD_createCCtx();
    s->drun = ZSTD_createDCtx();
    s->reset = 1;
    if (!s->enc || !s->chk || !s->cctx || !s->dctx || !s->zrun || !s->drun || !p->usable) {
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
    ZSTD_freeCCtx((ZSTD_CCtx*)s->zrun);
    ZSTD_freeDCtx((ZSTD_DCtx*)s->drun);
    rb_free(&s->sec);
    rb_free(&s->pk);
    rb_free(&s->back);
    rb_free(&s->out);
    rb_free(&s->prev);
    s->enc = s->chk = NULL;
    s->cctx = s->dctx = s->zrun = s->drun = NULL;
    s->chain = 0;
}

/* The container `sec` as the run's next chunk in `pk`, flushed so it ends at a zstd block's end;
   at a reset the run's context first restarts with the run's parameters (tagpu_recfmt.h, THE RUN
   AND THE CHAIN). RECC_SECTIONS; RECC_NOMEM when zstd or `pk` had no memory; RECC_MISMATCH when
   zstd failed otherwise. */
static int run_pack(RecStream* s, int reset)
{
    ZSTD_CCtx* c = (ZSTD_CCtx*)s->zrun;
    ZSTD_inBuffer in;
    ZSTD_outBuffer out;
    if (reset &&
        (ZSTD_isError(ZSTD_CCtx_reset(c, ZSTD_reset_session_and_parameters)) ||
         ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, RECC_LEVEL_MAX)) ||
         ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, REC_RUN_WINDOW_LOG)) ||
         ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_hashLog, RECC_HASH_LOG)) ||
         ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_chainLog, RECC_CHAIN_LOG)) ||
         ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_contentSizeFlag, 0)) ||
         ZSTD_isError(ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 0))))
        return RECC_MISMATCH;
    s->pk.n = 0;
    s->pk.oom = 0;
    if (!rb_grow(&s->pk, ZSTD_compressBound(s->sec.n))) return RECC_NOMEM;
    in.src = s->sec.p;
    in.size = s->sec.n;
    in.pos = 0;
    for (;;) {
        size_t left;
        out.dst = s->pk.p;
        out.size = s->pk.cap;
        out.pos = s->pk.n;
        left = ZSTD_compressStream2(c, &out, &in, ZSTD_e_flush);
        s->pk.n = out.pos;
        if (ZSTD_isError(left))
            return ZSTD_getErrorCode(left) == ZSTD_error_memory_allocation ? RECC_NOMEM
                                                                          : RECC_MISMATCH;
        if (!left) return RECC_SECTIONS;
        if (!rb_grow(&s->pk, left)) return RECC_NOMEM;
    }
}

/* DECODE BEFORE WRITE: the block is written sectioned only when the check decoder D, from the
   state a reader will have, gives back these records from the very bytes written. D is a second
   state, not the encoder's: the check has to start where a reader starts and must not share the
   encoder's arithmetic. The bytes written are a chunk of the run's frame, so the check unpacks
   them with a decoder of its own that has unpacked exactly the run's chunks before, as a reader's
   has; the chunk must give back the container byte for byte, then D its records.
   A records block, and every snapshot (reccodec_snapshot), make the next block restart both
   frames of both from the encoder's copies of the units, the pool and the grid, which are always
   the payload's end state, and begin a new run in both zstd contexts -- as does any failure here,
   zstd's or the codec's, so a run's chunks are exactly the sectioned blocks written since its
   reset block, in order. */
int reccodec_stream_block(RecStream* s, const unsigned char* payload, size_t n, unsigned* flags,
                          const unsigned char** body, size_t* bn)
{
    int k, reset = s->reset, packed;
    if (reset) {
        unsigned count, hw;
        const unsigned char* pool = reccodec_pool(s->enc, &count, &hw);
        RecFrameWorld fw;
        /* INVARIANT (an ordering): the grid handed over is the same bytes as every reader's at
           this reset. E's is the segment's images over zeros (E is made per segment, its grid
           zeroed: cells_make), applied in record order by the reader's rule -- an RK_CELLS with
           no RCS_EVENTS record before it in its iteration -- and each bounded as the reader's
           cells_ok bounds it; it takes every block's, coded or declined (plain_apply), so it is
           the payload's end state as the units and the pool are. D takes these bytes by copy
           (construction). A sequential reader builds its own by the same rule (construction;
           codectest's world_same checks E's against it on every transcoded block, and verify
           decodes against the records). A seeking reader's is a snapshot of the writer's copy
           over the segment's base, built by the same rule (construction): the copy takes an
           RK_CELLS only where every reader takes it as an image, since the writer refuses cells
           after an RCS_EVENTS record of their iteration outside the feature tick's call, and
           RK_GRID anywhere but before the first iteration's events (tagpu_recwriter.c,
           feature_record), and its base is the grid at the end of the segment's first
           iteration, as the reader's is (tagpu_recwriter.c, iteration_done; tagpu_recreader.c,
           tick_take); tarec.py check compares each snapshot's world with the stream's, grid
           included. No reader checks a decoded block against a checksum, so a fault in one of
           these rules would misdecode silently after a seek, as for the units. E's restart keeps
           its own grid (the world aliases it). */
        reccodec_world(s->enc, &fw);
        reccodec_restart(s->enc, &s->p, reccodec_units(s->enc), reccodec_live(s->enc), pool, count, hw, &fw);
        reccodec_restart(s->chk, &s->p, reccodec_units(s->enc), reccodec_live(s->enc), pool, count, hw, &fw);
    }
    /* THE TEST LEVER (RecStream.decline): an invalid encoder takes reccodec_encode's first
       branch -- plain_apply of the whole block, the encoder left invalid, RECC_DECLINED -- and
       then this function's records block and the next block's restart. It reaches no other
       decline: not the one in mid-block (enc_iteration fails, plain_apply from that iteration
       on), nor those after the walk (over, a column's oom, units_write, pool_write,
       frames_write), which return with nothing left to apply. No fixture's recording declines
       a block of itself, so those stay unexercised by the fixtures. */
    if (s->decline) {
        unsigned b = s->sectioned + s->declined + s->mismatched;
        if (b >= 2 && (b - 2) % 5 == 0) reccodec_invalidate(s->enc);
    }
    k = reccodec_encode(s->enc, payload, n, &s->sec);
    if (k == RECC_SECTIONS) {
        s->back.n = 0;
        s->back.oom = 0;
        k = run_pack(s, reset);
        if (k == RECC_SECTIONS && !rb_grow(&s->back, s->sec.n + 1)) k = RECC_NOMEM;
        if (k == RECC_SECTIONS &&
            (!reccodec_run_unpack(s->drun, s->back.p, s->sec.n, s->pk.p, s->pk.n, reset) ||
            memcmp(s->back.p, s->sec.p, s->sec.n) ||
            !reccodec_decode(s->chk, s->back.p, s->sec.n, &s->out, NULL, 0) || s->out.n != n ||
            memcmp(s->out.p, payload, n)))
            k = RECC_MISMATCH;
        if (k == RECC_SECTIONS) {
            *flags = RBF_SECTIONS | (reset ? RBF_FRAME_RESET : 0);
            *body = s->pk.p;
            *bn = s->pk.n;
            s->reset = 0;
            s->sectioned++;
            return k;
        }
    }
    if (k == RECC_MISMATCH) s->mismatched++;
    else s->declined++;
    if (k == RECC_NOMEM) s->nomem++;
    reccodec_invalidate(s->enc);
    reccodec_invalidate(s->chk);
    s->reset = 1;
    packed = reccodec_pack(s->cctx, &s->pk, payload, n);
    *flags = packed ? 0 : RBF_STORED;
    *body = packed ? s->pk.p : payload;
    *bn = packed ? s->pk.n : n;
    return k;
}

/* INVARIANTS (tagpu_recfmt.h, THE RUN AND THE CHAIN). DECODE BEFORE WRITE: a chained snapshot is
   written only when its frame, unpacked with the very prefix a reader will use, gives back its
   records byte for byte; a key, packed alone, only when it does too, else it is stored. THE
   PREFIX is the previous RB_SNAPSHOT of the segment: `prev` holds the records of the last
   snapshot this RecStream wrote, and one RecStream writes one segment (the writer's Writer and
   codectest's transcode each begin one at a header), so no snapshot of another segment is ever
   a prefix. THE CHAIN is bounded: after REC_CHAIN_MAX links the next snapshot is a key, as is the
   segment's first and any whose prefix and records pass 2^27 together or whose chained pack
   fails its check, so a reader never meets more than REC_CHAIN_MAX links. THE RUN: a snapshot
   ends it (reset), so the level-19 context is free to pack the snapshot. */
int reccodec_snapshot(RecStream* s, const unsigned char* raw, size_t n, unsigned* flags,
                      const unsigned char** body, size_t* bn)
{
    s->reset = 1;
    s->snapshots++;
    *flags = RBF_STORED;
    *body = raw;
    *bn = n;
    s->back.n = 0;
    s->back.oom = 0;
    if (s->chain && s->links < REC_CHAIN_MAX &&
        reccodec_pack_chained(s->zrun, &s->pk, raw, n, s->prev.p, s->prev.n) &&
        rb_grow(&s->back, n) &&
        reccodec_unpack_chained(s->dctx, s->back.p, n, s->pk.p, s->pk.n, s->prev.p, s->prev.n) &&
        !memcmp(s->back.p, raw, n)) {
        *flags = RBF_CHAINED;
        s->links++;
    } else {
        s->keys++;
        s->links = 0;
        s->back.n = 0;
        s->back.oom = 0;
        if (reccodec_pack_chained(s->zrun, &s->pk, raw, n, NULL, 0) && s->pk.n < n &&
            rb_grow(&s->back, n) && reccodec_unpack(s->dctx, s->back.p, n, s->pk.p, s->pk.n) &&
            !memcmp(s->back.p, raw, n))
            *flags = 0;
    }
    if (!(*flags & RBF_STORED)) {
        *body = s->pk.p;
        *bn = s->pk.n;
    }
    s->prev.n = 0;
    s->prev.oom = 0;
    rb_put(&s->prev, raw, n);
    s->chain = n && !s->prev.oom;
    return s->chain;
}
