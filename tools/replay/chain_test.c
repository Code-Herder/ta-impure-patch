/* The mip chains' C side (tagpu/ddraw/inc/tagpu_chain.h), compiled as the DLL compiles it and run
   natively by chain_test.py.

   WHAT IT HOLDS TO WHAT.
   (a) `tagpu_chain_ref`, which reduces every level from level 0 directly, against an emulation of
       the reducer's own order of work (src/tagpu_chain_comp.h REDUCE_CS: 64 invocations, level 1
       from 2 x 2 texels, level 2 from four level-1 sums, levels 3..5 from the shared sums), over
       random tiles: all opaque, all alpha 0, mixed alpha, and clipped valid sizes 1..32. The two
       must agree byte for byte, and an all-opaque tile's level must be its box mean rounded half
       up, an all-transparent one all zero.
   (b) `tagpu_chain_texel` -- the C mirror of the terrain fragment stage's taChain -- for L = 1..5
       over p in [-0.01, 32.07] (the edge nudge's range): every texel inside the cell's block, and
       for p in [0, 32) a block that holds floor(p), the texel level 0 samples.
   (c) `tagpu_chain_level` at the cases the spec names: 1x / 0.5x / 0.25x at ss = 2 are levels
       0 / 0 / 1; 0.034x is 4 at ss = 2 and 5 at ss = 1; 1/64 at ss = 1 is capped at 5; a NaN
       zoom is 0; an extent smaller than the game frame at 1x is at least 1.
   (d) THE FEATURES' TABLE FORM: a keyed frame of any size, cut into tiles from its first texel as
       tagpu_vk_feat.c's allocator cuts it, each tile reduced in the reducer's order from its rect
       and valid size, equals `tagpu_chain_ref` -- and stays the same when every texel outside the
       frame is scrambled, so no level reads another frame.
   (e) `tagpu_chain_ftexel` -- the C mirror of the feature fragment stage's taFChain -- for every
       texel of random frames at every level: the cell its tile was given, a level texel whose
       block holds the texel and at least one valid texel, and for a sample nudged past the frame's
       edge, a texel of the frame's own cells.

     chain_test [seed] [cases] */
#include "tagpu_chain.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("FAIL %u: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned s_rng;
static unsigned rnd(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }
static int rndi(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

static const int LOFF[TAGPU_CHAIN_LEVELS] = { 0, 256, 320, 336, 340 };

/* REDUCE_CS's taMean */
static void mean(const unsigned q[4], unsigned nb, unsigned char* o)
{
    unsigned c = q[3];
    o[0] = (unsigned char)(c ? (q[0] + c / 2) / c : 0);
    o[1] = (unsigned char)(c ? (q[1] + c / 2) / c : 0);
    o[2] = (unsigned char)(c ? (q[2] + c / 2) / c : 0);
    o[3] = (unsigned char)(nb ? (c * 255u + nb / 2) / nb : 0);
}

static void put(unsigned char* out, int L, int x, int y, const unsigned q[4], unsigned nb)
{
    int n = 32 >> L;
    mean(q, nb, out + (size_t)(LOFF[L - 1] + y * n + x) * 4);
}

static void add(unsigned a[4], const unsigned b[4])
{
    a[0] += b[0]; a[1] += b[1]; a[2] += b[2]; a[3] += b[3];
}

/* REDUCE_CS, invocation by invocation, barrier by barrier */
static void gpu(const unsigned char* rgba, int rowBytes, int sx, int sy, int tw, int th,
                unsigned char* out)
{
    unsigned s2[64][4], b2[64], s3[16][4], b3[16], s4[4][4], b4[4], q5[4] = { 0, 0, 0, 0 }, n5 = 0;
    int lx, ly, i, j, x, y, a;
    memset(s2, 0, sizeof s2); memset(b2, 0, sizeof b2);
    for (ly = 0; ly < 8; ly++)
        for (lx = 0; lx < 8; lx++) {
            unsigned q2[4] = { 0, 0, 0, 0 }, n2 = 0;
            for (j = 0; j < 2; j++)
                for (i = 0; i < 2; i++) {
                    unsigned q1[4] = { 0, 0, 0, 0 }, n1 = 0;
                    for (y = 0; y < 2; y++)
                        for (x = 0; x < 2; x++) {
                            int tx = lx * 4 + i * 2 + x, ty = ly * 4 + j * 2 + y;
                            const unsigned char* p;
                            if (tx >= tw || ty >= th) continue;
                            p = rgba + (size_t)(sy + ty) * rowBytes + (size_t)(sx + tx) * 4;
                            n1++;
                            if (p[3] >= 128) { q1[0] += p[0]; q1[1] += p[1]; q1[2] += p[2]; q1[3]++; }
                        }
                    put(out, 1, lx * 2 + i, ly * 2 + j, q1, n1);
                    add(q2, q1); n2 += n1;
                }
            put(out, 2, lx, ly, q2, n2);
            memcpy(s2[ly * 8 + lx], q2, sizeof q2); b2[ly * 8 + lx] = n2;
        }
    for (ly = 0; ly < 4; ly++)
        for (lx = 0; lx < 4; lx++) {
            unsigned q[4] = { 0, 0, 0, 0 };
            a = ly * 16 + lx * 2;
            add(q, s2[a]); add(q, s2[a + 1]); add(q, s2[a + 8]); add(q, s2[a + 9]);
            put(out, 3, lx, ly, q, b2[a] + b2[a + 1] + b2[a + 8] + b2[a + 9]);
            memcpy(s3[ly * 4 + lx], q, sizeof q);
            b3[ly * 4 + lx] = b2[a] + b2[a + 1] + b2[a + 8] + b2[a + 9];
        }
    for (ly = 0; ly < 2; ly++)
        for (lx = 0; lx < 2; lx++) {
            unsigned q[4] = { 0, 0, 0, 0 };
            a = ly * 8 + lx * 2;
            add(q, s3[a]); add(q, s3[a + 1]); add(q, s3[a + 4]); add(q, s3[a + 5]);
            put(out, 4, lx, ly, q, b3[a] + b3[a + 1] + b3[a + 4] + b3[a + 5]);
            memcpy(s4[ly * 2 + lx], q, sizeof q);
            b4[ly * 2 + lx] = b3[a] + b3[a + 1] + b3[a + 4] + b3[a + 5];
        }
    for (i = 0; i < 4; i++) { add(q5, s4[i]); n5 += b4[i]; }
    put(out, 5, 0, 0, q5, n5);
}

/* a source of `kind` at (sx, sy) of a 40 x 40 image: 0 all opaque, 1 all alpha 0, 2 random alpha,
   3 alpha 0 or 255 in runs (a keyed sprite's shape) */
#define SW 40
static unsigned char s_img[SW * SW * 4];
static void make(int kind)
{
    int i;
    for (i = 0; i < SW * SW; i++) {
        unsigned char* p = s_img + i * 4;
        p[0] = (unsigned char)rnd(); p[1] = (unsigned char)rnd(); p[2] = (unsigned char)rnd();
        p[3] = kind == 0 ? 255 : kind == 1 ? 0 : kind == 2 ? (unsigned char)rnd()
             : (((i % SW) / rndi(1, 5) + (i / SW) / 3) & 1) ? 255 : 0;
    }
}

static void test_ref(int cases)
{
    unsigned char a[TAGPU_CHAIN_TEXELS * 4], b[TAGPU_CHAIN_TEXELS * 4];
    int c, L, x, y, k;
    for (c = 0; c < cases; c++) {
        int kind = c % 4, clip = (c / 4) % 2;
        int tw = clip ? rndi(1, 32) : 32, th = clip ? rndi(1, 32) : 32;
        int sx = rndi(0, SW - 32), sy = rndi(0, SW - 32);
        make(kind);
        memset(a, 0xAA, sizeof a); memset(b, 0x55, sizeof b);
        tagpu_chain_ref(s_img, SW * 4, sx, sy, tw, th, a);
        gpu(s_img, SW * 4, sx, sy, tw, th, b);
        CHECK(!memcmp(a, b, sizeof a));
        if (kind == 1)
            for (k = 0; k < TAGPU_CHAIN_TEXELS * 4; k++) CHECK(a[k] == 0);
        if (kind == 0 && !clip)
            /* an opaque tile's level is its box mean, rounded half up, alpha 255 */
            for (L = 1; L <= TAGPU_CHAIN_LEVELS; L++) {
                int n = 32 >> L, s = 1 << L, ch;
                for (y = 0; y < n; y++)
                    for (x = 0; x < n; x++)
                        for (ch = 0; ch < 4; ch++) {
                            unsigned sum = 0, xx, yy;
                            for (yy = 0; yy < (unsigned)s; yy++)
                                for (xx = 0; xx < (unsigned)s; xx++)
                                    sum += s_img[((sy + y * s + yy) * SW + sx + x * s + xx) * 4 + ch];
                            CHECK(a[(LOFF[L - 1] + y * n + x) * 4 + ch] ==
                                  (unsigned char)((sum + (unsigned)(s * s) / 2) / (unsigned)(s * s)));
                        }
            }
    }
}

static void test_texel(void)
{
    int L, cx, cy, x, y, step;
    for (L = 1; L <= TAGPU_CHAIN_LEVELS; L++) {
        int n = 32 >> L;
        cx = rndi(0, 63); cy = rndi(0, 481);
        for (step = 0; step <= 32080; step++) {
            float p = -0.01f + (float)step * 0.001f;
            if (p > 32.07f) break;
            tagpu_chain_texel(L, cx, cy, p, 31.99f - p, &x, &y);
            CHECK(x >= cx * n && x < cx * n + n);
            CHECK(y >= cy * n && y < cy * n + n);
            if (p >= 0.0f && p < 32.0f) {
                int f = (int)floorf(p);
                CHECK((x - cx * n) << L <= f && f < (x - cx * n + 1) << L);
            }
            if (31.99f - p >= 0.0f && 31.99f - p < 32.0f) {
                int f = (int)floorf(31.99f - p);
                CHECK((y - cy * n) << L <= f && f < (y - cy * n + 1) << L);
            }
        }
    }
}

/* (d): frames in a 160 x 160 image */
#define FW 160
static unsigned char s_fimg[FW * FW * 4];
static void test_frame(int cases)
{
    unsigned char a[TAGPU_CHAIN_TEXELS * 4], b[TAGPU_CHAIN_TEXELS * 4];
    static unsigned char keep[400][TAGPU_CHAIN_TEXELS * 4];
    int c, i, tx, ty, t, x, y;
    for (c = 0; c < cases; c++) {
        int w = rndi(1, 100), h = rndi(1, 100), ox = rndi(1, FW - 101), oy = rndi(1, FW - 101);
        for (i = 0; i < FW * FW; i++) {
            unsigned char* p = s_fimg + i * 4;
            p[0] = (unsigned char)rnd(); p[1] = (unsigned char)rnd(); p[2] = (unsigned char)rnd();
            p[3] = (rnd() & 3) ? 255 : 0;
        }
        tx = (w + 31) >> 5; ty = (h + 31) >> 5;
        for (t = 0; t < tx * ty; t++) {
            int ax = t % tx, ay = t / tx;
            int sx = ox + 32 * ax, sy = oy + 32 * ay;
            int tw = w - 32 * ax < 32 ? w - 32 * ax : 32, th = h - 32 * ay < 32 ? h - 32 * ay : 32;
            tagpu_chain_ref(s_fimg, FW * 4, sx, sy, tw, th, a);
            gpu(s_fimg, FW * 4, sx, sy, tw, th, b);
            CHECK(!memcmp(a, b, sizeof a));
            memcpy(keep[t], a, sizeof a);
        }
        /* everything outside the frame, scrambled: no tile's chain moves */
        for (y = 0; y < FW; y++)
            for (x = 0; x < FW; x++)
                if (x < ox || x >= ox + w || y < oy || y >= oy + h) {
                    unsigned char* p = s_fimg + (y * FW + x) * 4;
                    p[0] = (unsigned char)rnd(); p[3] = (unsigned char)rnd();
                }
        for (t = 0; t < tx * ty; t++) {
            int ax = t % tx, ay = t / tx;
            int tw = w - 32 * ax < 32 ? w - 32 * ax : 32, th = h - 32 * ay < 32 ? h - 32 * ay : 32;
            gpu(s_fimg, FW * 4, ox + 32 * ax, oy + 32 * ay, tw, th, b);
            CHECK(!memcmp(keep[t], b, sizeof b));
        }
    }
}

/* (e) */
static void test_ftexel(int cases)
{
    const int dim = 2048;
    static const float fr[3] = { 0.01f, 0.5f, 0.99f };
    int c, L, ix, iy, k;
    for (c = 0; c < cases; c++) {
        int w = rndi(1, 200), h = rndi(1, 200), ox = rndi(1, dim - 201), oy = rndi(1, dim - 201);
        int tx = (w + 31) >> 5, ty = (h + 31) >> 5;
        int base = rndi(0, TAGPU_CHAIN_FCELLS - tx * ty);
        unsigned rec[4];
        rec[0] = (unsigned)ox | (unsigned)oy << 16;
        rec[1] = (unsigned)w | (unsigned)h << 16;
        rec[2] = (unsigned)base; rec[3] = (unsigned)tx;
        for (L = 1; L <= TAGPU_CHAIN_LEVELS; L++) {
            int n = 32 >> L;
            for (iy = -1; iy <= h; iy++)
                for (ix = -1; ix <= w; ix++)
                    for (k = 0; k < 3; k++) {
                        /* -1 and w (h) are samples the edge nudge carries just past the frame */
                        float u = ((float)(ox + ix) + (ix < 0 ? 0.99f : ix >= w ? 0.01f : fr[k])) / (float)dim;
                        float v = ((float)(oy + iy) + (iy < 0 ? 0.99f : iy >= h ? 0.01f : fr[k])) / (float)dim;
                        int jx = ix < 0 ? 0 : ix >= w ? w - 1 : ix, jy = iy < 0 ? 0 : iy >= h ? h - 1 : iy;
                        int cell = base + (jy >> 5) * tx + (jx >> 5), x, y, qx, qy;
                        int vw = w - (jx & ~31) < 32 ? w - (jx & ~31) : 32;
                        int vh = h - (jy & ~31) < 32 ? h - (jy & ~31) : 32;
                        tagpu_chain_ftexel(L, rec, u, v, dim, &x, &y);
                        CHECK(cell < TAGPU_CHAIN_FCELLS);
                        CHECK(x / n == cell % TAGPU_CHAIN_FCOLS && y / n == cell / TAGPU_CHAIN_FCOLS);
                        CHECK(x >= 0 && x < TAGPU_CHAIN_FCOLS * n && y >= 0 && y < TAGPU_CHAIN_FROWS * n);
                        qx = x % n; qy = y % n;
                        /* the block holds the texel, and so at least one valid texel */
                        CHECK(qx << L <= (jx & 31) && (jx & 31) < (qx + 1) << L);
                        CHECK(qy << L <= (jy & 31) && (jy & 31) < (qy + 1) << L);
                        CHECK(qx << L < vw && qy << L < vh);
                        if (ix >= 0 && ix < w && iy >= 0 && iy < h) continue;
                        k = 3;              /* one sample a nudged edge texel */
                    }
        }
    }
}

static void test_level(void)
{
    const float gw = 640.0f, gh = 480.0f;
    CHECK(tagpu_chain_level(1.0f, gw, gh, 1280, 960) == 0);
    CHECK(tagpu_chain_level(0.5f, gw, gh, 1280, 960) == 0);
    CHECK(tagpu_chain_level(0.25f, gw, gh, 1280, 960) == 1);
    CHECK(tagpu_chain_level(0.034f, gw, gh, 1280, 960) == 4);
    CHECK(tagpu_chain_level(0.034f, gw, gh, 640, 480) == 5);
    CHECK(tagpu_chain_level(1.0f / 64.0f, gw, gh, 640, 480) == 5);
    CHECK(tagpu_chain_level(nanf(""), gw, gh, 1280, 960) == 0);
    CHECK(tagpu_chain_level(1.0f, gw, gh, 320, 240) >= 1);
    /* the smaller axis decides: a target squeezed in one direction only */
    CHECK(tagpu_chain_level(1.0f, gw, gh, 1280, 240) == 1);
    /* no game frame is garbage, not a division by zero */
    CHECK(tagpu_chain_level(1.0f, 0.0f, gh, 1280, 960) == 0);
    /* at and above 1x with the target standing, every ss is level 0 */
    {
        int ss;
        for (ss = 1; ss <= 8; ss++) {
            CHECK(tagpu_chain_level(1.0f, gw, gh, 640u * (unsigned)ss, 480u * (unsigned)ss) == 0);
            CHECK(tagpu_chain_level(4.0f, gw, gh, 640u * (unsigned)ss, 480u * (unsigned)ss) == 0);
            CHECK(tagpu_chain_level(1.0f / (float)ss, gw, gh, 640u * (unsigned)ss, 480u * (unsigned)ss) == 0);
        }
    }
}

int main(int argc, char** argv)
{
    unsigned seed = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 1u;
    int cases = argc > 2 ? atoi(argv[2]) : 4000;
    s_rng = seed;
    test_ref(cases);
    printf("ref: %d tiles, the reducer's order equals the direct reduction\n", cases);
    test_texel();
    printf("texel: every level's fetch inside its block, and holding floor(p) on [0, 32)\n");
    test_level();
    printf("level: every case as the spec names it\n");
    test_frame(cases / 10 > 0 ? cases / 10 : 1);
    printf("frame: every tile of a keyed frame reduces from the frame alone, as the reference does\n");
    test_ftexel(cases / 20 > 0 ? cases / 20 : 1);
    printf("ftexel: every texel of every frame finds its own cell, and a block that holds it\n");
    printf("every check holds\n");
    return 0;
}
