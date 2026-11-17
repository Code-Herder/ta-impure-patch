/* recdifftest.c -- inc/tagpu_recdiff.h's rec_diff_from against the plain answer.

   `reference` is the capture's comparison as it stood before the SSE2 one, 16 bytes a step in
   general-purpose registers and then a byte loop; it and rec_diff_from must give the same index
   for every input. The cases:

   - every length 0..200, every start 0..n+1 (past the end included), every single differing byte,
     on three alignment pairs of the two buffers;
   - every length 0..96 on all 256 alignment pairs, every single differing byte, the starts around
     it;
   - the lengths the capture asks (a unit record, a mover, a unit image, the declared tables'
     rows), and the block edges 511..4097, with one differing byte at every position;
   - seeded random buffers up to 8 KB with differences at four densities, before and after the
     start, each a random bit, the sign bit included.

   Each buffer ends where its allocation does, so a read past `n` is out of bounds: the host build
   (tests/recdifftest.py) runs under AddressSanitizer. `make recdifftest` is the Windows build, the
   DLL's compiler and flags; `recdifftest.exe bench` also times both on buffers shaped as one
   capture's. Exit 0 when nothing failed. */
#define _POSIX_C_SOURCE 199309L   /* clock_gettime under -std=c99 on the host */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_recdiff.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

static unsigned reference(const unsigned char* a, const unsigned char* b, unsigned i, unsigned n)
{
    while (i + 16 <= n) {
        uint32_t x[4], y[4];
        memcpy(x, a + i, 16);
        memcpy(y, b + i, 16);
        if ((x[0] ^ y[0]) | (x[1] ^ y[1]) | (x[2] ^ y[2]) | (x[3] ^ y[3])) break;
        i += 16;
    }
    while (i < n && a[i] == b[i]) i++;
    return i;
}

static unsigned long long s_rng = 0x9E3779B97F4A7C15ull;
static unsigned rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (unsigned)(s_rng >> 16);
}

static unsigned long s_checks, s_fails;

static void check(const unsigned char* a, const unsigned char* b, unsigned i, unsigned n,
                  const char* what)
{
    unsigned want = reference(a, b, i, n), got = rec_diff_from(a, b, i, n);
    s_checks++;
    if (want != got && s_fails++ < 20)
        printf("FAIL %s: n=%u i=%u a%%16=%u b%%16=%u: reference %u, rec_diff_from %u\n", what, n, i,
               (unsigned)((uintptr_t)a & 15), (unsigned)((uintptr_t)b & 15), want, got);
}

/* A buffer of n bytes `al` (0..15) past the start of its allocation, which ends where the buffer
   does. As `al` takes all sixteen values, the buffer takes all sixteen alignments mod 16 whatever
   malloc's own alignment is (16 under AddressSanitizer, so there `al` is the alignment itself). */
typedef struct { unsigned char* mem; unsigned char* p; } Buf;

static Buf buf(unsigned n, unsigned al)
{
    Buf r;
    r.mem = (unsigned char*)malloc(n + al ? n + al : 1);
    if (!r.mem) { printf("FAIL out of memory\n"); exit(2); }
    r.p = r.mem + al;
    return r;
}

static void fill(unsigned char* p, unsigned n)
{
    unsigned k;
    for (k = 0; k < n; k++) p[k] = (unsigned char)rnd();
}

/* the bit a single difference flips: every one of the eight in turn */
static unsigned char flip(unsigned j) { return (unsigned char)(1u << (j & 7)); }

static void single_diffs(unsigned n, unsigned al, unsigned bl, int every_start)
{
    Buf A = buf(n, al), B = buf(n, bl);
    unsigned i, j;
    fill(A.p, n);
    memcpy(B.p, A.p, n);
    for (i = 0; i <= n + 1; i++) check(A.p, B.p, i, n, "equal");
    for (j = 0; j < n; j++) {
        B.p[j] ^= flip(j);
        if (every_start) {
            for (i = 0; i <= n + 1; i++) check(A.p, B.p, i, n, "one byte");
        } else {
            unsigned starts[] = { 0, 1, 15, 16, 17, 63, 64, 65, j ? j - 1 : 0, j, j + 1, n };
            unsigned k;
            for (k = 0; k < sizeof starts / sizeof *starts; k++)
                check(A.p, B.p, starts[k], n, "one byte");
        }
        B.p[j] ^= flip(j);
    }
    free(A.mem);
    free(B.mem);
}

static void random_cases(unsigned trials)
{
    static const unsigned per_mille[] = { 1, 10, 100, 500 };
    unsigned t;
    for (t = 0; t < trials; t++) {
        unsigned n = rnd() % 8193, k, i;
        unsigned dens = per_mille[t & 3];
        Buf A = buf(n, rnd() & 15), B = buf(n, rnd() & 15);
        fill(A.p, n);
        memcpy(B.p, A.p, n);
        for (k = 0; k < n; k++)
            if (rnd() % 1000 < dens) B.p[k] ^= (unsigned char)(1u << (rnd() & 7));
        for (k = 0; k < 16; k++) {
            i = n ? rnd() % (n + 2) : rnd() % 2;
            check(A.p, B.p, i, n, "random");
        }
        /* walk it as rows_moved does: from each difference to the next row's start */
        for (i = 0; (i = rec_diff_from(A.p, B.p, i, n)) < n; i = (i / 80 + 1) * 80)
            check(A.p, B.p, i, n, "walk");
        free(A.mem);
        free(B.mem);
    }
}

/* ---- the benchmark ---------------------------------------------------------------------------- */

static double now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
#endif
}

typedef unsigned (*DiffFn)(const unsigned char*, const unsigned char*, unsigned, unsigned);

static volatile unsigned s_sink;

/* One capture's comparisons, sized as the raised build's: the static declared tables against their
   copies, walked as rows_moved walks them (`rows_changed` rows of 80 bytes differ), 1500 live units'
   records and movers against their copies (half of them moved, at a random byte), and 16384 rows
   of 140 zero bytes tested against zeros as row_emit tests them. */
typedef struct {
    unsigned char *ta, *tb, *ua, *ub, *zr, *zz;
    unsigned tbytes, units, zrows;
    unsigned* uat;
} Shape;

static double pass(DiffFn f, const Shape* s, int part)
{
    double t0 = now_us();
    unsigned at = 0, k, acc = 0;
    if (part == 0)
        while ((at = f(s->ta, s->tb, at, s->tbytes)) < s->tbytes) at = (at / 80 + 1) * 80;
    if (part == 1)
        for (k = 0; k < s->units; k++) {
            const unsigned char* a = s->ua + (size_t)k * 0x148;
            const unsigned char* b = s->ub + (size_t)k * 0x148;
            if (f(a, b, 0, 0x118) == 0x118) acc += f(a + 0x118, b + 0x118, 0, 0x2F);
        }
    if (part == 2)
        for (k = 0; k < s->zrows; k++) acc += f(s->zr + (size_t)k * 140, s->zz, 0, 140);
    s_sink = acc + at;
    return now_us() - t0;
}

static void bench(void)
{
    static const char* const name[3] = { "static tables, 6 MB, 200 rows moved",
                                         "1500 live units, half moved",
                                         "16384 zero rows of 140 bytes" };
    Shape s;
    unsigned k, part, rep;
    s.tbytes = 6u << 20;
    s.units = 1500;
    s.zrows = 16384;
    s.ta = (unsigned char*)malloc(s.tbytes);
    s.tb = (unsigned char*)malloc(s.tbytes);
    s.ua = (unsigned char*)malloc((size_t)s.units * 0x148);
    s.ub = (unsigned char*)malloc((size_t)s.units * 0x148);
    s.zr = (unsigned char*)calloc(s.zrows, 140);
    s.zz = (unsigned char*)calloc(1, 140);
    if (!s.ta || !s.tb || !s.ua || !s.ub || !s.zr || !s.zz) { printf("FAIL out of memory\n"); exit(2); }
    fill(s.ta, s.tbytes);
    memcpy(s.tb, s.ta, s.tbytes);
    for (k = 0; k < 200; k++) s.tb[rnd() % s.tbytes] ^= 1;
    fill(s.ua, s.units * 0x148);
    memcpy(s.ub, s.ua, (size_t)s.units * 0x148);
    for (k = 0; k < s.units; k += 2) s.ub[(size_t)k * 0x148 + rnd() % (0x118 + 0x2F)] ^= 1;
    for (part = 0; part < 3; part++) {
        double best[2] = { 1e30, 1e30 };
        for (rep = 0; rep < 200; rep++) {
            double a = pass(reference, &s, part), b = pass(rec_diff_from, &s, (int)part);
            if (a < best[0]) best[0] = a;
            if (b < best[1]) best[1] = b;
        }
        printf("bench %-38s reference %8.1f us, rec_diff_from %8.1f us, %.2fx\n", name[part],
               best[0], best[1], best[0] / best[1]);
    }
}

int main(int argc, char** argv)
{
    unsigned n, al, bl;
    static const unsigned pairs[3][2] = { { 0, 0 }, { 1, 3 }, { 15, 8 } };
    static const unsigned sizes[] = { 0x2F, 0x118, 0x148, 140, 364, 680, 4404, 7168, 7501,
                                      511, 512, 513, 1023, 1024, 1025, 4095, 4096, 4097 };
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        bench();
        return 0;
    }
    for (n = 0; n <= 200; n++)
        for (al = 0; al < 3; al++) single_diffs(n, pairs[al][0], pairs[al][1], 1);
    for (n = 0; n <= 96; n++)
        for (al = 0; al < 16; al++)
            for (bl = 0; bl < 16; bl++) single_diffs(n, al, bl, 0);
    for (n = 0; n < sizeof sizes / sizeof *sizes; n++) single_diffs(sizes[n], n & 15, 0, 0);
    random_cases(20000);
    printf("%s: %lu checks, %lu failed\n", s_fails ? "FAIL" : "PASS", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
